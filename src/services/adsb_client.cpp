#include "services/adsb_client.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <ArduinoJson.h>

#include <cstring>

#include "config.h"
#include "services/radar_location.h"
#include "ui/radar_range.h"

namespace services::adsb {

namespace {

constexpr char kApiBase[] = "https://opendata.adsb.fi/api/v3/lat/";
constexpr float kKmPerNm = 1.852f;
constexpr int kConnectAttemptMs = 200;
constexpr unsigned long kRequestTimeoutMs = 10000;
constexpr uint8_t kMaxGetAttempts = 3;
constexpr unsigned long kRetryPauseMs = 50;

/**
 * Two buffers: the task parses into the back one and swaps under the mutex, so
 * the render loop never reads a half-written list and never blocks on the fetch.
 */
Aircraft s_buffers[2][kMaxAircraft];
size_t s_counts[2] = {0, 0};
uint8_t s_front = 0;
SemaphoreHandle_t s_mutex = nullptr;
TaskHandle_t s_task = nullptr;
unsigned long s_last_update_ms = 0;

/** Big enough for the mbedTLS handshake; high-water mark checked on device. */
constexpr uint32_t kFetchTaskStackBytes = 8192;

/** Dead reckoning past this is guesswork, so positions freeze instead. */
constexpr float kMaxExtrapolationSec = 12.0f;
constexpr float kDegToRad = 0.01745329252f;
constexpr float kKnotsToKmPerSec = kKmPerNm / 3600.0f;

/**
 * A couple of quick retries cover the moment just after the link comes up.
 * This used to retry until a 10 s deadline with only a 5 ms pause, which meant
 * ~118 TLS handshakes against an API documented at 1 req/s -- so one transient
 * failure could get the address throttled and turn itself into an outage. The
 * fetch task comes back in a few seconds regardless, so giving up early is
 * strictly better than hammering.
 */
int performGetWithRetry(HTTPClient& http) {
  http.setConnectTimeout(kConnectAttemptMs);
  for (uint8_t attempt = 0; attempt < kMaxGetAttempts; ++attempt) {
    const int code = http.GET();
    if (code > 0) {
      return code;
    }
    if (code != HTTPC_ERROR_CONNECTION_REFUSED &&
        code != HTTPC_ERROR_NOT_CONNECTED) {
      return code;
    }
    delay(kRetryPauseMs);
  }
  return HTTPC_ERROR_CONNECTION_REFUSED;
}

float kmToNauticalMiles(float km) { return km / kKmPerNm; }

/** Make a freshly parsed buffer visible to the render loop. */
void publish(uint8_t back, size_t count) {
  s_counts[back] = count;
  if (s_mutex != nullptr) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
  }
  s_front = back;
  s_last_update_ms = millis();
  if (s_mutex != nullptr) {
    xSemaphoreGive(s_mutex);
  }
}

/**
 * The only fields this client reads. adsb.fi returns ~53 per aircraft; parsing
 * all of them peaks at ~32 KB of heap for a typical response and was failing
 * with NoMemory. A filter still scans the whole body but only allocates for
 * these keys, which measures ~9.6 KB for the same payload.
 */
constexpr const char* kWantedFields[] = {
    "lat",  "lon", "true_heading", "mag_heading", "track",    "dir",
    "gs",   "tas", "ias",          "alt_baro",    "alt_geom", "flight",
    "hex",  "t",   "dst",         "seen_pos"};

void buildFilter(JsonDocument& filter) {
  JsonObject plane = filter["ac"][0].to<JsonObject>();
  for (const char* key : kWantedFields) {
    plane[key] = true;
  }
}

bool readJsonFloat(const JsonObject& obj, const char* key, float* out) {
  if (obj[key].is<float>() || obj[key].is<double>() || obj[key].is<int>()) {
    *out = obj[key].as<float>();
    return true;
  }
  return false;
}

float pickNoseHeading(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "true_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "mag_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "track", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "dir", &v)) {
    return v;
  }
  return 0.0f;
}

float pickTrackHeading(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "track", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "true_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "mag_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "dir", &v)) {
    return v;
  }
  return 0.0f;
}

float pickGroundSpeed(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "gs", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "tas", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "ias", &v)) {
    return v;
  }
  return 0.0f;
}

bool isOnGround(const JsonObject& plane) {
  if (!plane["alt_baro"].is<const char*>()) {
    return false;
  }
  return strcmp(plane["alt_baro"].as<const char*>(), "ground") == 0;
}

void copyJsonStringTrimmed(const JsonObject& obj, const char* key, char* out,
                           size_t out_len) {
  out[0] = '\0';
  if (out_len == 0 || !obj[key].is<const char*>()) {
    return;
  }
  const char* s = obj[key].as<const char*>();
  size_t n = strnlen(s, out_len - 1);
  while (n > 0 && s[n - 1] == ' ') {
    --n;
  }
  memcpy(out, s, n);
  out[n] = '\0';
}

void formatAltitudeTag(const JsonObject& plane, char* out, size_t out_len) {
  out[0] = '\0';
  if (out_len == 0) {
    return;
  }

  if (plane["alt_baro"].is<const char*>()) {
    const char* s = plane["alt_baro"].as<const char*>();
    if (strcmp(s, "ground") == 0) {
      strncpy(out, "GND", out_len - 1);
      out[out_len - 1] = '\0';
      return;
    }
  }

  float alt = 0.0f;
  if (readJsonFloat(plane, "alt_baro", &alt) ||
      readJsonFloat(plane, "alt_geom", &alt)) {
    snprintf(out, out_len, "%d ft", static_cast<int>(lroundf(alt)));
  }
}

void fillTagFields(Aircraft* ac, const JsonObject& plane) {
  copyJsonStringTrimmed(plane, "flight", ac->callsign, sizeof(ac->callsign));
  if (ac->callsign[0] == '\0') {
    copyJsonStringTrimmed(plane, "hex", ac->callsign, sizeof(ac->callsign));
  }

  copyJsonStringTrimmed(plane, "t", ac->type, sizeof(ac->type));
  formatAltitudeTag(plane, ac->alt, sizeof(ac->alt));
}

}  // namespace

size_t aircraftCount() { return s_counts[s_front]; }

const Aircraft* aircraftList() { return s_buffers[s_front]; }

bool hasTraffic() { return s_counts[s_front] > 0; }

bool aircraftLock(uint32_t timeout_ms) {
  if (s_mutex == nullptr) {
    return true;  // task never started: single-threaded, nothing to guard
  }
  return xSemaphoreTake(s_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void aircraftUnlock() {
  if (s_mutex != nullptr) {
    xSemaphoreGive(s_mutex);
  }
}

bool fetchUpdate(double center_lat, double center_lon, float fetch_radius_km) {
  const float dist_nm = kmToNauticalMiles(fetch_radius_km);

  String url = kApiBase;
  url += String(center_lat, 6);
  url += "/lon/";
  url += String(center_lon, 6);
  url += "/dist/";
  url += String(dist_nm, 1);

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, url)) {
    Serial.println("adsb: http.begin failed");
    return false;
  }

  http.setTimeout(kRequestTimeoutMs);
  const int code = performGetWithRetry(http);
  if (code != HTTP_CODE_OK) {
    Serial.printf("adsb: HTTP %d\n", code);
    http.end();
    return false;
  }

  WiFiClient* body = http.getStreamPtr();
  if (body == nullptr) {
    Serial.println("adsb: no response stream");
    http.end();
    return false;
  }

  JsonDocument filter;
  buildFilter(filter);

  JsonDocument doc;
  const DeserializationError err =
      deserializeJson(doc, *body, DeserializationOption::Filter(filter));
  http.end();
  client.stop();
  if (err) {
    Serial.printf("adsb: JSON parse error: %s (heap=%u largest=%u)\n",
                  err.c_str(), ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    return false;
  }

  const uint8_t back = s_front ^ 1;
  Aircraft* out = s_buffers[back];

  JsonArray ac = doc["ac"].as<JsonArray>();
  if (ac.isNull()) {
    publish(back, 0);
    return true;
  }

  size_t n = 0;
  for (JsonObject plane : ac) {
    if (n >= kMaxAircraft) {
      break;
    }
    if (!plane["lat"].is<float>() || !plane["lon"].is<float>()) {
      continue;
    }
    if (isOnGround(plane) && !config::kAdsbShowGroundAircraft) {
      continue;
    }

    out[n].lat = plane["lat"].as<float>();
    out[n].lon = plane["lon"].as<float>();
    out[n].nose_deg = pickNoseHeading(plane);
    out[n].track_deg = pickTrackHeading(plane);
    out[n].gs_knots = pickGroundSpeed(plane);
    // Resolve the track into east/north components now: it is constant until
    // the next fetch, and the render loop runs many frames per fetch.
    const float gs_km_s = out[n].gs_knots * kKnotsToKmPerSec;
    const float track_rad = out[n].track_deg * kDegToRad;
    out[n].vel_e_km_s = gs_km_s * sinf(track_rad);
    out[n].vel_n_km_s = gs_km_s * cosf(track_rad);
    float dst = -1.0f;
    out[n].dst_nm = readJsonFloat(plane, "dst", &dst) ? dst : -1.0f;
    float seen_pos = 0.0f;
    out[n].pos_age_s =
        readJsonFloat(plane, "seen_pos", &seen_pos) ? seen_pos : 0.0f;
    fillTagFields(&out[n], plane);
    ++n;
  }

  publish(back, n);
  Serial.printf("adsb: %u aircraft\n", static_cast<unsigned>(n));
  return true;
}

namespace {

void fetchTask(void*) {
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      fetchUpdate(services::location::lat(), services::location::lon(),
                  ui::radar::fetchRadiusKm());
    }
    vTaskDelay(pdMS_TO_TICKS(config::kAdsbFetchIntervalMs));
  }
}

}  // namespace

bool startFetchTask() {
  if (s_task != nullptr) {
    return true;
  }
  s_mutex = xSemaphoreCreateMutex();
  if (s_mutex == nullptr) {
    Serial.println("adsb: mutex alloc failed");
    return false;
  }
  // Same priority as the Arduino loop task: the fetch spends nearly all its
  // time blocked on the socket, so the render loop runs while it waits.
  if (xTaskCreate(fetchTask, "adsb", kFetchTaskStackBytes, nullptr, 1, &s_task) !=
      pdPASS) {
    Serial.println("adsb: fetch task create failed");
    s_task = nullptr;
    return false;
  }
  return true;
}

unsigned fetchTaskStackFree() {
  return s_task == nullptr
             ? 0
             : uxTaskGetStackHighWaterMark(s_task) * sizeof(StackType_t);
}

float secondsSinceUpdate() {
  if (s_last_update_ms == 0) {
    return 0.0f;
  }
  const float age_s = (millis() - s_last_update_ms) / 1000.0f;
  return age_s > kMaxExtrapolationSec ? kMaxExtrapolationSec : age_s;
}

}  // namespace services::adsb
