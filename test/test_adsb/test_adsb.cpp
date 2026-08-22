// End-to-end exercise of the real adsb_client.cpp: URL building, the JSON
// filter, field extraction and fallbacks, ground filtering, velocity
// pre-computation, the double-buffer publish, staleness and expiry, the retry
// cap, and every error path.
#include <Arduino.h>
#include <unity.h>
#include <cmath>
#include <cstring>

#include "fixtures_adsb.h"
#include "../mocks/mock_globals.h"
#include "../../src/services/radar_location.cpp"
#include "../../src/ui/radar_range.cpp"
#include "../../src/ui/radar_geo.cpp"
#include "../../src/services/adsb_client.cpp"

using namespace services::adsb;

static bool fetch(const char* payload, int code = HTTP_CODE_OK) {
  g_http.reset();
  g_http.body = payload ? payload : "";
  g_http.code = code;
  return fetchUpdate(40.445564, -3.698361, 30.0f);
}

void setUp() {
  g_nvs.reset(); mockSetMs(100000); g_tls = MockTlsStats();
  WiFi.status_ = WL_CONNECTED;
  services::location::clear(); ui::radar::rangeInit();
}
void tearDown() {}

// ---------- the JSON filter must not drop a field the parser reads ----------
static void test_filter_covers_every_field_the_parser_reads() {
  // These are the keys read by pickNoseHeading/pickTrackHeading/pickGroundSpeed/
  // isOnGround/formatAltitudeTag/fillTagFields plus position, dst and seen_pos.
  const char* consumed[] = {"lat","lon","true_heading","mag_heading","track","dir",
                            "gs","tas","ias","alt_baro","alt_geom","flight","hex",
                            "t","dst","seen_pos"};
  for (const char* key : consumed) {
    bool found = false;
    for (const char* f : kWantedFields) if (strcmp(f, key) == 0) { found = true; break; }
    char msg[96];
    snprintf(msg, sizeof(msg), "'%s' is read by the parser but missing from kWantedFields", key);
    TEST_ASSERT_TRUE_MESSAGE(found, msg);
  }
}

static void test_url_is_built_from_centre_and_radius() {
  fetch(kAirbornePayload());
  TEST_ASSERT_TRUE(strstr(g_http.last_url.c_str(), "/lat/40.445564") != nullptr);
  TEST_ASSERT_TRUE(strstr(g_http.last_url.c_str(), "/lon/-3.698361") != nullptr);
  TEST_ASSERT_TRUE_MESSAGE(strstr(g_http.last_url.c_str(), "/dist/16.2") != nullptr,
                           "radius must be converted km -> nautical miles");
}

// ---------- parsing real payloads ----------
static void test_real_payload_parses_every_aircraft() {
  TEST_ASSERT_TRUE(fetch(kAirbornePayload()));
  TEST_ASSERT_EQUAL_INT_MESSAGE(kAirborneCount, (int)aircraftCount(),
                                "every airborne record should survive the filter");
  const Aircraft* a = aircraftList();
  for (size_t i = 0; i < aircraftCount(); ++i) {
    TEST_ASSERT_TRUE_MESSAGE(a[i].lat > 30.0f && a[i].lat < 60.0f,
                             "latitude must be the payload's, not garbage");
    TEST_ASSERT_TRUE_MESSAGE(a[i].lon > -20.0f && a[i].lon < 20.0f,
                             "longitude must be the payload's, not garbage");
    TEST_ASSERT_TRUE_MESSAGE(a[i].callsign[0] != '\0', "every target needs an identity");
  }
}

static void test_real_ground_traffic_is_filtered_out() {
  TEST_ASSERT_TRUE(fetch(kGroundMixPayload()));
  TEST_ASSERT_EQUAL_INT_MESSAGE(kGroundMixAirborne, (int)aircraftCount(),
      "every alt_baro=\"ground\" record in this real capture must be hidden");
}

static void test_ground_targets_and_positionless_targets_are_dropped() {
  TEST_ASSERT_TRUE(fetch(kTrickyPayload()));
  // 5 in the payload: one on the ground, one with no lat/lon -> 3 remain.
  TEST_ASSERT_EQUAL_INT(3, (int)aircraftCount());
  const Aircraft* a = aircraftList();
  for (size_t i = 0; i < aircraftCount(); ++i)
    TEST_ASSERT_TRUE_MESSAGE(strncmp(a[i].callsign, "GND1", 4) != 0,
                             "an aircraft reporting alt_baro=ground must be hidden");
}

static void test_callsign_is_trimmed_and_falls_back_to_hex() {
  fetch(kTrickyPayload());
  const Aircraft* a = aircraftList();
  TEST_ASSERT_EQUAL_STRING_MESSAGE("TEST123", a[0].callsign, "trailing pad spaces must go");
  bool found_hex_fallback = false;
  for (size_t i = 0; i < aircraftCount(); ++i)
    if (strcmp(a[i].callsign, "nofl") == 0) found_hex_fallback = true;
  TEST_ASSERT_TRUE_MESSAGE(found_hex_fallback, "no flight field -> hex is used as identity");
}

static void test_heading_and_speed_fallback_chains() {
  fetch(kTrickyPayload());
  const Aircraft* a = aircraftList();
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 91.0f, a[0].nose_deg);   // true_heading wins
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 90.0f, a[0].track_deg);  // track wins
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 300.0f, a[0].gs_knots);
  for (size_t i = 0; i < aircraftCount(); ++i) {
    if (strcmp(a[i].callsign, "nofl") != 0) continue;
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, 45.0f, a[i].nose_deg, "falls back to mag_heading");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, 250.0f, a[i].gs_knots, "falls back to tas");
  }
}

static void test_altitude_tag_formatting() {
  fetch(kTrickyPayload());
  const Aircraft* a = aircraftList();
  TEST_ASSERT_EQUAL_STRING("2000 ft", a[0].alt);
  for (size_t i = 0; i < aircraftCount(); ++i)
    if (strcmp(a[i].callsign, "nofl") == 0)
      TEST_ASSERT_EQUAL_STRING_MESSAGE("5000 ft", a[i].alt, "alt_geom is the fallback");
}

static void test_missing_optional_fields_do_not_corrupt_the_record() {
  fetch(kTrickyPayload());
  const Aircraft* a = aircraftList();
  for (size_t i = 0; i < aircraftCount(); ++i) {
    if (strcmp(a[i].callsign, "minimal") != 0) continue;
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", a[i].type, "absent type must be an empty string");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", a[i].alt, "absent altitude must be an empty string");
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, a[i].gs_knots);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, 0.0f, a[i].pos_age_s, "absent seen_pos -> 0");
  }
}

// ---------- dead reckoning inputs ----------
static void test_velocity_is_resolved_at_fetch_time() {
  fetch(kTrickyPayload());
  const Aircraft* a = aircraftList();
  // 300 kt due east (track 90) -> all easting, no northing.
  const float kmps = 300.0f * 1.852f / 3600.0f;
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.001f, kmps, a[0].vel_e_km_s, "east component");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.001f, 0.0f, a[0].vel_n_km_s, "no north component");
}

static void test_seen_pos_is_captured_for_fix_age_anchoring() {
  fetch(kTrickyPayload());
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, 0.4f, aircraftList()[0].pos_age_s,
      "without seen_pos, a stale repeated fix snaps the target backwards");
}

// ---------- staleness and expiry ----------
static void test_age_is_clamped_to_the_horizon() {
  fetch(kAirbornePayload());
  mockAdvanceMs(60UL * 1000UL);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, kExtrapolationHorizonSec, secondsSinceUpdate());
  TEST_ASSERT_TRUE_MESSAGE(secondsSinceUpdateRaw() > 50.0f, "the raw age must NOT be clamped");
}

static void test_data_expires_and_hides_traffic() {
  fetch(kAirbornePayload());
  TEST_ASSERT_TRUE(hasTraffic());
  TEST_ASSERT_FALSE(dataExpired());
  mockAdvanceMs(59UL * 1000UL);
  TEST_ASSERT_FALSE_MESSAGE(dataExpired(), "still inside the 60 s window");
  TEST_ASSERT_TRUE(hasTraffic());
  mockAdvanceMs(2UL * 1000UL);
  TEST_ASSERT_TRUE_MESSAGE(dataExpired(), "past 60 s with no fetch");
  TEST_ASSERT_FALSE_MESSAGE(hasTraffic(),
      "expired data must stop claiming traffic, or the panel shows dead aircraft as live");
}

static void test_a_successful_fetch_revives_expired_data() {
  fetch(kAirbornePayload());
  mockAdvanceMs(120UL * 1000UL);
  TEST_ASSERT_TRUE(dataExpired());
  fetch(kAirbornePayload());
  TEST_ASSERT_FALSE(dataExpired());
  TEST_ASSERT_TRUE(hasTraffic());
}

static void test_age_is_zero_before_the_first_fetch() {
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, secondsSinceUpdateRaw());
  TEST_ASSERT_FALSE_MESSAGE(dataExpired(), "never-fetched is not the same as expired");
}

// ---------- error paths ----------
static void test_http_error_does_not_publish_and_drops_the_session() {
  fetch(kAirbornePayload());
  const size_t before = aircraftCount();
  g_tls = MockTlsStats();
  TEST_ASSERT_FALSE(fetch(kAirbornePayload(), 429));
  TEST_ASSERT_EQUAL_INT_MESSAGE((int)before, (int)aircraftCount(),
      "a failed fetch must leave the previous list intact");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_tls.stop,
      "exactly one teardown: a second stop() on a torn-down client closes fd 0");
}

// Keep-alive is the whole point of the file-scope client: a success that tears
// the session down reintroduces the ~33 KB per-cycle reallocation.
static void test_success_keeps_the_session_open() {
  g_tls = MockTlsStats();
  TEST_ASSERT_TRUE(fetch(kAirbornePayload()));
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_tls.stop,
      "a successful fetch must NOT drop the connection");
}

// Every error path must route through stopSession(), or the open/closed flag
// desynchronises and the next teardown double-stops.
static void test_every_error_path_stops_exactly_once() {
  const struct { const char* what; const char* body; int code; } cases[] = {
      {"http error", nullptr, 429},
      {"garbage body", "definitely not json", HTTP_CODE_OK},
      {"truncated body", "{\"ac\":[{\"lat\":40.1,\"lon\":-3.1", HTTP_CODE_OK},
  };
  for (const auto& c : cases) {
    fetch(kAirbornePayload());              // re-open a session
    g_tls = MockTlsStats();
    fetch(c.body ? c.body : kAirbornePayload(), c.code);
    char msg[96];
    snprintf(msg, sizeof(msg), "%s: expected exactly one stop", c.what);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_tls.stop, msg);
    // A follow-up teardown must be a no-op: the session is already closed.
    stopSession();
    snprintf(msg, sizeof(msg), "%s: a redundant teardown must be suppressed", c.what);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_tls.stop, msg);
  }
}

static void test_truncated_body_is_rejected_rather_than_half_parsed() {
  std::string full = kAirbornePayload();
  g_http.reset();
  g_http.body = full.substr(0, full.size() / 2);   // cut mid-document
  g_http.content_length_override = (int)full.size();
  TEST_ASSERT_FALSE_MESSAGE(fetchUpdate(40.4, -3.6, 30.0f),
      "a truncated document must fail, not publish partial traffic");
}

static void test_garbage_body_is_rejected() {
  TEST_ASSERT_FALSE(fetch("not json at all"));
}

// A filter makes ArduinoJson skip what it does not want, so a non-API body can
// parse "Ok" into an empty document. Treating that as an empty sky wipes real
// traffic off the panel AND refreshes the update timestamp, so the 60 s expiry
// never fires: the radar sits blank while looking healthy.
static void test_non_api_bodies_are_not_mistaken_for_an_empty_sky() {
  const char* bodies[] = {"<html>502 Bad Gateway</html>", "null", "12345",
                          "[1,2,3]", "definitely not json"};
  for (const char* b : bodies) {
    fetch(kAirbornePayload());
    const size_t good = aircraftCount();
    TEST_ASSERT_TRUE(good > 0);
    char msg[128];
    snprintf(msg, sizeof(msg), "body %s must be rejected, not read as 'no aircraft'", b);
    TEST_ASSERT_FALSE_MESSAGE(fetch(b), msg);
    snprintf(msg, sizeof(msg), "body %s must leave the last good list intact", b);
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)good, (int)aircraftCount(), msg);
  }
}

// ...but a real empty sky must still clear the panel.
static void test_a_genuine_empty_response_still_clears() {
  fetch(kAirbornePayload());
  TEST_ASSERT_TRUE(aircraftCount() > 0);
  TEST_ASSERT_TRUE(fetch("{\"ac\":[],\"msg\":\"No error\",\"total\":0}"));
  TEST_ASSERT_EQUAL_INT(0, (int)aircraftCount());
}

// A response with no 'ac' array is not this API. Treating it as an empty sky
// would clear real traffic AND refresh the update timestamp, so the 60 s expiry
// would never fire either.
static void test_a_missing_ac_key_is_rejected_not_read_as_empty() {
  fetch(kAirbornePayload());
  const size_t good = aircraftCount();
  TEST_ASSERT_TRUE(good > 0);
  TEST_ASSERT_FALSE_MESSAGE(fetch("{\"msg\":\"No error\"}"),
      "no 'ac' array means the response was not the aircraft feed");
  TEST_ASSERT_EQUAL_INT_MESSAGE((int)good, (int)aircraftCount(),
      "and the last good list must survive");
}

static void test_empty_ac_array_publishes_zero_not_stale_traffic() {
  fetch(kAirbornePayload());
  TEST_ASSERT_TRUE(aircraftCount() > 0);
  TEST_ASSERT_TRUE(fetch("{\"ac\":[]}"));
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)aircraftCount(),
      "an empty sky must publish zero so the clearing frame is drawn");
}

// The retry cap exists because ~118 handshakes in 10 s got the address throttled.
static void test_retries_are_capped() {
  g_http.reset();
  g_http.body = kAirbornePayload();
  g_http.fail_first_n_gets = 99;
  fetchUpdate(40.4, -3.6, 30.0f);
  TEST_ASSERT_EQUAL_INT_MESSAGE(3, g_http.get_calls,
      "exactly the cap: fewer means no retry at all, more means a storm "
      "against an API documented at 1 req/s");
}

// The teardown flag must only be armed once a socket really exists. Armed at
// begin() time, a connect that never succeeded still triggers stop(), and
// ssl_client leaves socket == 0 after its own cleanup -- so that stop() is a
// close(0) on the console descriptor.
static void test_a_failed_connect_never_closes_fd0() {
  g_http.reset();
  g_http.body = kAirbornePayload();
  g_http.fail_first_n_gets = 99;          // every attempt refused: no socket
  g_tls = MockTlsStats();
  TEST_ASSERT_FALSE(fetchUpdate(40.4, -3.6, 30.0f));
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_tls.close_of_fd0,
      "tearing down a connection that was never opened closes fd 0");
}

// And after a real connection, exactly one teardown -- never two.
static void test_error_after_a_real_connection_stops_once_only() {
  fetch(kAirbornePayload());               // opens a session
  g_tls = MockTlsStats();
  TEST_ASSERT_FALSE(fetch("<html>502</html>"));
  TEST_ASSERT_EQUAL_INT(1, g_tls.stop);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_tls.close_of_fd0, "first stop had a real fd");
  stopSession();                           // a later link-down
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_tls.stop, "suppressed: already closed");
  TEST_ASSERT_EQUAL_INT(0, g_tls.close_of_fd0);
}

static void test_a_transient_failure_still_succeeds_within_the_cap() {
  g_http.reset();
  g_http.body = kAirbornePayload();
  g_http.fail_first_n_gets = 2;      // two blips, then fine
  TEST_ASSERT_TRUE(fetchUpdate(40.4, -3.6, 30.0f));
  TEST_ASSERT_EQUAL_INT(3, g_http.get_calls);
}

// ---------- capacity ----------
static void test_aircraft_cap_is_respected() {
  std::string big = "{\"ac\":[";
  for (int i = 0; i < (int)kMaxAircraft + 25; ++i) {
    if (i) big += ",";
    char b[192];
    snprintf(b, sizeof(b), "{\"hex\":\"a%04d\",\"lat\":40.%03d,\"lon\":-3.6,\"gs\":200,\"track\":90}", i, i % 999);
    big += b;
  }
  big += "]}";
  TEST_ASSERT_TRUE(fetch(big.c_str()));
  TEST_ASSERT_EQUAL_INT_MESSAGE((int)kMaxAircraft, (int)aircraftCount(),
      "the fixed buffer must cap rather than overflow");
  // Independent bound: kMaxAircraft sizes two static Aircraft buffers, the tag
  // rect array, and two arrays on the loop task's stack. Reading it on both
  // sides above pins that a cap exists but never its value.
  TEST_ASSERT_EQUAL_INT_MESSAGE(64, (int)kMaxAircraft,
      "changing this silently resizes two static buffers and ~2 KB of stack "
      "in drawAircraft -- update the memory notes in CLAUDE.md if it moves");
}

// --------------------------------------------- task and link lifecycle -----

// ~33 KB of mbedTLS state stays pinned across a WiFi outage unless the session
// is released on the down transition. A fetch is ~0.5 s of a ~3.5 s cycle, so
// the link almost always drops BETWEEN requests, where no error path runs.
static void test_the_tls_session_is_released_when_the_link_drops() {
  fetch(kAirbornePayload());                 // a session is open
  bool was_connected = true;
  g_tls = MockTlsStats();
  fetchTick(/*link_up=*/false, &was_connected);
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_tls.stop,
      "a link that drops between fetches must still free the TLS buffers");
  TEST_ASSERT_FALSE(was_connected);
}

static void test_a_link_that_stays_down_is_not_torn_down_repeatedly() {
  fetch(kAirbornePayload());
  bool was_connected = true;
  g_tls = MockTlsStats();
  for (int i = 0; i < 5; ++i) fetchTick(false, &was_connected);
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_tls.stop,
      "only the transition releases; repeated down ticks must be no-ops");
}

static void test_no_teardown_when_the_link_was_never_up() {
  bool was_connected = false;
  g_tls = MockTlsStats();
  fetchTick(false, &was_connected);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_tls.stop, "nothing was ever opened");
}

static void test_a_tick_while_connected_fetches() {
  g_http.reset(); g_http.body = kAirbornePayload(); g_http.code = HTTP_CODE_OK;
  bool was_connected = false;
  fetchTick(true, &was_connected);
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_http.get_calls, "a connected tick must fetch");
  TEST_ASSERT_TRUE(was_connected);
}

// loop() retries startFetchTask() every 10 s while it has not succeeded, so the
// failure path must be both reportable and leak-free.
static void test_task_creation_failure_is_reported_and_retryable() {
  g_task_create_fail = 1;
  TEST_ASSERT_FALSE_MESSAGE(startFetchTask(), "a failed xTaskCreate must be reported");
  TEST_ASSERT_TRUE_MESSAGE(startFetchTask(),
      "the retry must succeed -- the failure path has to release the mutex, or "
      "every retry strands another one");
}

static void test_mutex_allocation_failure_is_reported() {
  g_mutex_alloc_fail = 1;
  TEST_ASSERT_FALSE_MESSAGE(startFetchTask(), "no mutex means no safe handoff");
}

static void test_starting_the_task_twice_is_idempotent() {
  TEST_ASSERT_TRUE(startFetchTask());
  TEST_ASSERT_TRUE_MESSAGE(startFetchTask(),
      "loop() calls this repeatedly; it must not spawn a second 8 KB task");
}

// The filter is why parsing fits in the heap budget. Asserting only that the
// field list is right would not notice it being dropped from the call.
static void test_unwanted_fields_are_not_retained() {
  std::string p = "{\"ac\":[{\"hex\":\"aa\",\"lat\":40.4,\"lon\":-3.6,\"gs\":200,"
                  "\"track\":90,\"junk\":\"";
  p += std::string(3000, 'x');            // a field far larger than the record
  p += "\"}]}";
  TEST_ASSERT_TRUE(fetch(p.c_str()));
  TEST_ASSERT_EQUAL_INT(1, (int)aircraftCount());
  // Nothing in Aircraft can hold 3 KB, so this only proves parsing survived.
  // The real guard is that the filter kept the document small enough to fit.
  TEST_ASSERT_EQUAL_STRING_MESSAGE("aa", aircraftList()[0].callsign,
      "the wanted fields must still be extracted alongside a huge unwanted one");
}

// A non-retryable error must return at once rather than burn all three attempts
// against an API documented at 1 req/s.
static void test_a_non_retryable_error_does_not_consume_the_retry_budget() {
  g_http.reset();
  g_http.body = kAirbornePayload();
  g_http.code = 500;                       // a real response, just not OK
  TEST_ASSERT_FALSE(fetchUpdate(40.4, -3.6, 30.0f));
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_http.get_calls,
      "a server error is not a connection failure -- retrying it is pure load");
}

// The shape guard has two arms; only the first was exercised.
static void test_an_ac_field_that_is_not_an_array_is_rejected() {
  fetch(kAirbornePayload());
  const size_t good = aircraftCount();
  TEST_ASSERT_TRUE(good > 0);
  for (const char* body : {"{\"ac\":5}", "{\"ac\":\"oops\"}", "{\"ac\":{\"a\":1}}"}) {
    char m[128];
    snprintf(m, sizeof(m), "body %s must not be read as an empty sky", body);
    TEST_ASSERT_FALSE_MESSAGE(fetch(body), m);
    snprintf(m, sizeof(m), "body %s must leave the last good list intact", body);
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)good, (int)aircraftCount(), m);
    fetch(kAirbornePayload());             // restore for the next case
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  // These three must run before anything creates the task or the mutex:
  // startFetchTask() is idempotent by design, so once it has succeeded the
  // failure paths become unreachable.
  RUN_TEST(test_mutex_allocation_failure_is_reported);
  RUN_TEST(test_task_creation_failure_is_reported_and_retryable);
  RUN_TEST(test_starting_the_task_twice_is_idempotent);
  // Must run first among the age tests: s_last_update_ms is file-static.
  RUN_TEST(test_age_is_zero_before_the_first_fetch);
  RUN_TEST(test_filter_covers_every_field_the_parser_reads);
  RUN_TEST(test_url_is_built_from_centre_and_radius);
  RUN_TEST(test_real_payload_parses_every_aircraft);
  RUN_TEST(test_real_ground_traffic_is_filtered_out);
  RUN_TEST(test_ground_targets_and_positionless_targets_are_dropped);
  RUN_TEST(test_callsign_is_trimmed_and_falls_back_to_hex);
  RUN_TEST(test_heading_and_speed_fallback_chains);
  RUN_TEST(test_altitude_tag_formatting);
  RUN_TEST(test_missing_optional_fields_do_not_corrupt_the_record);
  RUN_TEST(test_velocity_is_resolved_at_fetch_time);
  RUN_TEST(test_seen_pos_is_captured_for_fix_age_anchoring);
  RUN_TEST(test_age_is_clamped_to_the_horizon);
  RUN_TEST(test_data_expires_and_hides_traffic);
  RUN_TEST(test_a_successful_fetch_revives_expired_data);
  RUN_TEST(test_http_error_does_not_publish_and_drops_the_session);
  RUN_TEST(test_success_keeps_the_session_open);
  RUN_TEST(test_every_error_path_stops_exactly_once);
  RUN_TEST(test_truncated_body_is_rejected_rather_than_half_parsed);
  RUN_TEST(test_garbage_body_is_rejected);
  RUN_TEST(test_non_api_bodies_are_not_mistaken_for_an_empty_sky);
  RUN_TEST(test_a_genuine_empty_response_still_clears);
  RUN_TEST(test_empty_ac_array_publishes_zero_not_stale_traffic);
  RUN_TEST(test_a_missing_ac_key_is_rejected_not_read_as_empty);
  RUN_TEST(test_retries_are_capped);
  RUN_TEST(test_a_failed_connect_never_closes_fd0);
  RUN_TEST(test_error_after_a_real_connection_stops_once_only);
  RUN_TEST(test_a_transient_failure_still_succeeds_within_the_cap);
  RUN_TEST(test_aircraft_cap_is_respected);
  RUN_TEST(test_the_tls_session_is_released_when_the_link_drops);
  RUN_TEST(test_a_link_that_stays_down_is_not_torn_down_repeatedly);
  RUN_TEST(test_no_teardown_when_the_link_was_never_up);
  RUN_TEST(test_a_tick_while_connected_fetches);
  RUN_TEST(test_unwanted_fields_are_not_retained);
  RUN_TEST(test_a_non_retryable_error_does_not_consume_the_retry_budget);
  RUN_TEST(test_an_ac_field_that_is_not_an_array_is_rejected);
  return UNITY_END();
}
