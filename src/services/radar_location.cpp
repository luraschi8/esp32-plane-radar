#include "services/radar_location.h"

#include "debug_log.h"

#include <Preferences.h>
#include <cstdlib>
#include <cstring>

#include "config.h"

namespace services::location {

namespace {

constexpr char kPrefsNamespace[] = "radar";
constexpr char kKeyLat[] = "lat";
constexpr char kKeyLon[] = "lon";

double s_lat = config::kDefaultRadarLat;
double s_lon = config::kDefaultRadarLon;
portMUX_TYPE s_coord_mux = portMUX_INITIALIZER_UNLOCKED;

bool parseCoord(const char* text, double* out) {
  if (text == nullptr || text[0] == '\0') {
    return false;
  }
  char* end = nullptr;
  const double v = strtod(text, &end);
  if (end == text || (end != nullptr && *end != '\0')) {
    return false;
  }
  *out = v;
  return true;
}

bool validLatLon(double lat, double lon) {
  return lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0;
}

/** False when NVS refused the write; the runtime value is still updated. */
bool persist(double lat, double lon) {
  Preferences prefs;
  // putDouble() is a silent no-op on a handle that failed to open, so an
  // unchecked begin() meant the portal reported "saved" while the coordinates
  // were lost on the next reboot.
  const bool opened = prefs.begin(kPrefsNamespace, false);
  if (opened) {
    prefs.putDouble(kKeyLat, lat);
    prefs.putDouble(kKeyLon, lon);
    prefs.end();
  } else {
    Serial.println("radar: NVS unavailable — location not persisted");
  }
  portENTER_CRITICAL(&s_coord_mux);
  s_lat = lat;
  s_lon = lon;
  portEXIT_CRITICAL(&s_coord_mux);
  return opened;
}

}  // namespace

void init() {
  Preferences prefs;
  // A read-only open of a namespace that has never been written logs
  // "nvs_open failed: NOT_FOUND" from the framework. That is expected on a
  // first boot and is not an error; DEBUG_LOG says which way it went.
  [[maybe_unused]] const bool opened = prefs.begin(kPrefsNamespace, true);
  DEBUG_LOG("location: nvs namespace '%s' %s", kPrefsNamespace,
            opened ? "opened" : "absent (first boot or after a reset)");
  if (prefs.isKey(kKeyLat) && prefs.isKey(kKeyLon)) {
    const double lat = prefs.getDouble(kKeyLat, config::kDefaultRadarLat);
    const double lon = prefs.getDouble(kKeyLon, config::kDefaultRadarLon);
    if (validLatLon(lat, lon)) {
      s_lat = lat;
      s_lon = lon;
    }
  }
  prefs.end();
  DEBUG_LOG("location: centre %.6f, %.6f (%s)", s_lat, s_lon,
            opened ? "from NVS" : "compiled-in default");
}

double lat() { return s_lat; }

double lon() { return s_lon; }

void snapshot(double* out_lat, double* out_lon) {
  portENTER_CRITICAL(&s_coord_mux);
  *out_lat = s_lat;
  *out_lon = s_lon;
  portEXIT_CRITICAL(&s_coord_mux);
}

bool saveFromStrings(const char* lat_str, const char* lon_str) {
  double lat = 0.0;
  double lon = 0.0;
  if (!parseCoord(lat_str, &lat) || !parseCoord(lon_str, &lon)) {
    return false;
  }
  if (!validLatLon(lat, lon)) {
    return false;
  }
  if (!persist(lat, lon)) {
    // The coordinates WERE accepted and are live; only the NVS write failed.
    // Reporting that as a rejection made the caller print "keeping previous
    // location" directly under this line, which is the opposite of the truth.
    Serial.printf("Radar location applied but NOT saved: %.6f, %.6f\n", lat, lon);
    return true;
  }
  Serial.printf("Radar location saved: %.6f, %.6f\n", lat, lon);
  return true;
}

void clear() {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, false);
  prefs.remove(kKeyLat);
  prefs.remove(kKeyLon);
  prefs.end();
  portENTER_CRITICAL(&s_coord_mux);
  s_lat = config::kDefaultRadarLat;
  s_lon = config::kDefaultRadarLon;
  portEXIT_CRITICAL(&s_coord_mux);
}

}  // namespace services::location
