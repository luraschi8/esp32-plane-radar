// Real radar_range.cpp and radar_location.cpp against an in-memory NVS.
#include <Arduino.h>
#include <unity.h>
#include <cmath>
#include <cstring>

#include "../mocks/mock_globals.h"
#include "debug_log.h"
#include "../../src/services/radar_location.cpp"
#include "../../src/ui/radar_range.cpp"

using namespace ui::radar;

/**
 * The suites include the shipped .cpp files, so their file-statics persist
 * across tests. Without an explicit reset a test can pass because of what the
 * previous one left behind -- give each one a known starting state.
 */
static void resetSettingsState() {
  g_nvs.reset();
  services::location::clear();          // restores the compiled-in defaults
  Preferences seed;                     // create the namespace so rangeInit()
  seed.begin("planeradar", false);      // takes its normal (not first-boot) path
  seed.putUChar("rangeIdx", 0);
  seed.end();
  ui::radar::rangeInit();
}

void setUp() { mockSetMs(1000); resetSettingsState(); }
void tearDown() {}

// ---------------- range presets ----------------

static void test_presets_are_sorted_and_labelled_uniquely() {
  TEST_ASSERT_TRUE_MESSAGE(kRangePresetCount >= 2 && kRangePresetCount <= 8,
      "the BOOT-tap cycle has to stay short enough to be usable");
  char km[12], mi[12];
  const char* seen_km[8]; const char* seen_mi[8];
  static char kmbuf[8][12], mibuf[8][12];
  for (size_t i = 0; i < kRangePresetCount; ++i) {
    if (i > 0) TEST_ASSERT_TRUE_MESSAGE(kRangePresets[i].ring3_km > kRangePresets[i-1].ring3_km,
                                        "presets must ascend so the BOOT cycle reads naturally");
    // outer radius is ring-3 distance / 0.75
    TEST_ASSERT_FLOAT_WITHIN(0.01f, kRangePresets[i].ring3_km * 4.0f / 3.0f,
                             kRangePresets[i].outer_km);
    formatRing3Label(kmbuf[i], sizeof(kmbuf[i]), kRangePresets[i].ring3_km, false);
    formatRing3Label(mibuf[i], sizeof(mibuf[i]), kRangePresets[i].ring3_km, true);
    seen_km[i] = kmbuf[i]; seen_mi[i] = mibuf[i];
  }
  // Duplicate labels would make two different ranges indistinguishable on screen.
  for (size_t i = 0; i < kRangePresetCount; ++i)
    for (size_t j = i + 1; j < kRangePresetCount; ++j) {
      TEST_ASSERT_TRUE_MESSAGE(strcmp(seen_km[i], seen_km[j]) != 0, "duplicate km label");
      TEST_ASSERT_TRUE_MESSAGE(strcmp(seen_mi[i], seen_mi[j]) != 0, "duplicate mi label");
    }
  (void)km; (void)mi;
}

static void test_labels_render_expected_text() {
  char b[12];
  formatRing3Label(b, sizeof(b), 20.0f, false); TEST_ASSERT_EQUAL_STRING("20km", b);
  formatRing3Label(b, sizeof(b), 20.0f, true);  TEST_ASSERT_EQUAL_STRING("12mi", b);
  formatRing3Label(b, sizeof(b), 5.0f,  true);  TEST_ASSERT_EQUAL_STRING("3mi", b);
  formatRing3Label(b, sizeof(b), 25.0f, true);  TEST_ASSERT_EQUAL_STRING("16mi", b);
}

static void test_label_buffer_is_never_overrun() {
  char b[5];  // deliberately tight
  formatRing3Label(b, sizeof(b), 25.0f, false);
  TEST_ASSERT_TRUE(strlen(b) < sizeof(b));
}

static void test_one_tap_advances_exactly_one_preset() {
  for (size_t i = 0; i + 1 < kRangePresetCount; ++i) {
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(kRangePresets[i].ring3_km, rangeCurrent().ring3_km,
                                    "unexpected starting preset");
    rangeNext();
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(kRangePresets[i + 1].ring3_km, rangeCurrent().ring3_km,
                                    "one tap must move to the very next preset");
  }
}

static void test_tap_cycles_and_wraps() {
  const float first = rangeCurrent().ring3_km;
  for (size_t i = 0; i < kRangePresetCount; ++i) rangeNext();
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(first, rangeCurrent().ring3_km,
                                  "a full cycle must return to the start");
}

static void test_preset_survives_a_reboot() {
  rangeNext(); rangeNext();
  const float chosen = rangeCurrent().ring3_km;
  rangeInit();                                   // simulate a power cycle
  TEST_ASSERT_EQUAL_FLOAT(chosen, rangeCurrent().ring3_km);
}

// The 20 km preset was inserted mid-array, so a saved index means a different
// distance than it did before. Out-of-range indices must clamp, not crash.
// On a factory-fresh device the namespace does not exist, so the read-only
// open in rangeInit() fails and the compiled-in defaults stand.
static void test_first_boot_with_empty_nvs_uses_defaults() {
  g_nvs.reset();
  rangeInit();                                   // fresh-NVS path
  TEST_ASSERT_FALSE_MESSAGE(g_nvs.namespaceExists("planeradar"),
      "a failed read-only open must not create the namespace as a side effect");
  TEST_ASSERT_TRUE_MESSAGE(rangeCurrent().ring3_km > 0.0f,
                           "a first boot must still produce a usable range");
  // The first tap is what creates the namespace.
  rangeNext();
  TEST_ASSERT_TRUE_MESSAGE(g_nvs.namespaceExists("planeradar"),
                           "the first save must create the namespace");
}

static void test_saved_index_out_of_range_falls_back_to_default() {
  Preferences p; p.begin("planeradar", false); p.putUChar("rangeIdx", 200); p.end();
  rangeInit();
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(10.0f, rangeCurrent().ring3_km,
                                  "a bogus saved index must fall back to the 10 km default");
}

static void test_fetch_radius_exceeds_the_ring_so_rim_dots_have_data() {
  for (size_t i = 0; i < kRangePresetCount; ++i) {
    TEST_ASSERT_TRUE_MESSAGE(fetchRadiusKm() > rangeCurrent().outer_km,
        "fetch must reach past the outer ring or beyond-ring dots have no source");
    rangeNext();
  }
}

// ---------------- portal checkboxes ----------------

static void test_checkbox_parsing() {
  // An unchecked HTML checkbox submits nothing at all.
  TEST_ASSERT_FALSE_MESSAGE(portalCheckboxChecked(""), "absent field means unchecked");
  TEST_ASSERT_FALSE(portalCheckboxChecked(nullptr));
  TEST_ASSERT_TRUE(portalCheckboxChecked("T"));
  TEST_ASSERT_TRUE(portalCheckboxChecked("on"));
}

// Both toggles are checked at their NON-default value, and against the NVS
// store itself -- asserting a value that equals the default would pass whether
// or not anything was ever written.
static void test_units_and_runways_are_actually_persisted() {
  saveMilesFromPortal("T");                       // default is km
  saveRunwaysFromPortal("");                      // default is ON
  TEST_ASSERT_TRUE(useMiles());
  TEST_ASSERT_FALSE(showRunways());
  TEST_ASSERT_TRUE_MESSAGE(g_nvs.store.count("planeradar/useMiles"),
                           "miles must reach NVS, not just the runtime value");
  TEST_ASSERT_TRUE_MESSAGE(g_nvs.store.count("planeradar/showRwys"),
                           "runway toggle must reach NVS");
  rangeInit();                                    // reload from storage
  TEST_ASSERT_TRUE_MESSAGE(useMiles(), "miles must survive a reboot");
  TEST_ASSERT_FALSE_MESSAGE(showRunways(), "runways-off must survive a reboot");
}

static void test_reset_restores_both_toggles_and_clears_storage() {
  saveMilesFromPortal("T");
  saveRunwaysFromPortal("");
  unitsReset();
  TEST_ASSERT_FALSE_MESSAGE(useMiles(), "reset returns to km");
  TEST_ASSERT_TRUE_MESSAGE(showRunways(), "reset returns runways to ON");
  TEST_ASSERT_FALSE_MESSAGE(g_nvs.store.count("planeradar/useMiles"),
                            "reset must remove the key, not just the value");
  TEST_ASSERT_FALSE_MESSAGE(g_nvs.store.count("planeradar/showRwys"),
                            "reset must remove the key, not just the value");
}

// A BOOT-hold reset clears units and runways but NOT the range preset.
static void test_reset_preserves_the_range_preset() {
  rangeNext(); rangeNext();
  const float chosen = rangeCurrent().ring3_km;
  unitsReset();
  rangeInit();
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(chosen, rangeCurrent().ring3_km,
      "documented behaviour: a credential reset leaves the range alone");
}

// A failed NVS open makes putX() a silent no-op on real hardware, so an
// unchecked begin() would lose the coordinates on reboot with no sign of it.
// The return value means "accepted", not "persisted": returning false here made
// the portal print "keeping previous location" directly under radar_location's
// own "applied but NOT saved", which is the opposite of the truth.
static void test_a_refused_nvs_write_still_applies_the_location() {
  Serial.capture = true; Serial.log.clear();
  g_nvs.open_fail_count = 99;
  TEST_ASSERT_TRUE_MESSAGE(services::location::saveFromStrings("41.0", "-4.0"),
      "the coordinates were valid and are live; only the write failed");
  TEST_ASSERT_EQUAL_DOUBLE_MESSAGE(41.0, services::location::lat(),
      "the runtime value must apply for this session");
  TEST_ASSERT_FALSE_MESSAGE(g_nvs.store.count("radar/lat"), "nothing should be stored");
  char lm[256];
  snprintf(lm, sizeof(lm), "serial log was: '%s'", Serial.log.c_str());
  TEST_ASSERT_TRUE_MESSAGE(Serial.log.find("NOT saved") != std::string::npos, lm);
  TEST_ASSERT_TRUE_MESSAGE(Serial.log.find("keeping previous") == std::string::npos,
      "must not also claim the previous location was kept -- it was not");
  Serial.capture = false; Serial.log.clear();
}

// Invalid input is the case that DOES report failure, so the two are not
// conflated back together.
static void test_invalid_coordinates_are_rejected_and_change_nothing() {
  services::location::saveFromStrings("41.0", "-4.0");
  const double before_lat = services::location::lat();
  for (const char* bad_lat : {"91.0", "abc", "", "-91"}) {
    char m[112];
    snprintf(m, sizeof(m), "lat '%s' must be rejected", bad_lat);
    TEST_ASSERT_FALSE_MESSAGE(services::location::saveFromStrings(bad_lat, "-4.0"), m);
  }
  TEST_ASSERT_FALSE_MESSAGE(services::location::saveFromStrings("41.0", "181.0"),
      "an out-of-range longitude must be rejected");
  TEST_ASSERT_EQUAL_DOUBLE_MESSAGE(before_lat, services::location::lat(),
      "a rejected save must leave the previous location untouched");
}

// radar_location reports a refused NVS write rather than failing silently;
// radar_range used to swallow it, so a portal save looked successful and the
// setting was gone after a reboot. Same bug class, so same behaviour.
static void test_a_refused_nvs_write_still_applies_the_range_settings() {
  rangeInit();
  const float before = rangeCurrent().ring3_km;
  const bool miles_before = useMiles();
  const std::string stored_idx = g_nvs.store["planeradar/rangeIdx"];
  Serial.capture = true; Serial.log.clear();
  g_nvs.open_fail_count = 99;
  rangeNext();
  saveMilesFromPortal(miles_before ? "" : "T");
  TEST_ASSERT_TRUE_MESSAGE(rangeCurrent().ring3_km != before,
      "the range must still change for this session");
  TEST_ASSERT_TRUE_MESSAGE(useMiles() != miles_before,
      "and so must the unit setting");
  // setUp() seeds rangeIdx, so the key exists; what matters is that the failed
  // write did not change it.
  TEST_ASSERT_TRUE_MESSAGE(stored_idx == g_nvs.store["planeradar/rangeIdx"],
      "the stored preset must be untouched when the write was refused");
  TEST_ASSERT_FALSE_MESSAGE(g_nvs.store.count("planeradar/useMiles"),
      "and the unit setting must not have been written either");
  // The serial log is the only channel that can tell a user their setting will
  // not survive a reboot; without it the failure is completely silent.
  char m[256];
  snprintf(m, sizeof(m), "serial log was: '%s'", Serial.log.c_str());
  TEST_ASSERT_TRUE_MESSAGE(Serial.log.find("NOT saved") != std::string::npos, m);
  Serial.capture = false; Serial.log.clear();
}

static void test_nvs_open_failure_leaves_the_button_working() {
  const float before = rangeCurrent().ring3_km;
  g_nvs.open_fail_count = 99;
  rangeNext();
  TEST_ASSERT_TRUE_MESSAGE(rangeCurrent().ring3_km != before,
      "the range must still change even when it cannot be saved");
}

// ---------------- location ----------------

static void test_location_defaults_before_anything_is_saved() {
  services::location::init();
  TEST_ASSERT_EQUAL_DOUBLE(config::kDefaultRadarLat, services::location::lat());
  TEST_ASSERT_EQUAL_DOUBLE(config::kDefaultRadarLon, services::location::lon());
}

static void test_location_round_trips_through_nvs() {
  TEST_ASSERT_TRUE(services::location::saveFromStrings("40.445564", "-3.698361"));
  services::location::init();
  TEST_ASSERT_DOUBLE_WITHIN(1e-6, 40.445564, services::location::lat());
  TEST_ASSERT_DOUBLE_WITHIN(1e-6, -3.698361, services::location::lon());
}

static void test_location_rejects_bad_input_and_keeps_the_old_value() {
  services::location::saveFromStrings("40.0", "-3.0");
  const double lat = services::location::lat();
  TEST_ASSERT_FALSE(services::location::saveFromStrings("91.0", "0"));      // lat > 90
  TEST_ASSERT_FALSE(services::location::saveFromStrings("0", "181.0"));     // lon > 180
  TEST_ASSERT_FALSE(services::location::saveFromStrings("abc", "0"));       // not a number
  TEST_ASSERT_FALSE(services::location::saveFromStrings("40.0x", "0"));     // trailing junk
  TEST_ASSERT_FALSE(services::location::saveFromStrings("", "0"));
  TEST_ASSERT_FALSE(services::location::saveFromStrings(nullptr, "0"));
  TEST_ASSERT_EQUAL_DOUBLE_MESSAGE(lat, services::location::lat(),
                                   "a rejected save must not disturb the stored value");
}

static void test_location_accepts_the_extremes() {
  TEST_ASSERT_TRUE(services::location::saveFromStrings("90.0", "180.0"));
  TEST_ASSERT_TRUE(services::location::saveFromStrings("-90.0", "-180.0"));
  TEST_ASSERT_TRUE(services::location::saveFromStrings("0", "0"));
}

static void test_snapshot_returns_a_matching_pair() {
  services::location::saveFromStrings("51.4700", "-0.4543");
  double la = 0, lo = 0;
  services::location::snapshot(&la, &lo);
  TEST_ASSERT_EQUAL_DOUBLE(services::location::lat(), la);
  TEST_ASSERT_EQUAL_DOUBLE(services::location::lon(), lo);
}

static void test_clear_restores_defaults() {
  services::location::saveFromStrings("51.47", "-0.45");
  services::location::clear();
  TEST_ASSERT_EQUAL_DOUBLE(config::kDefaultRadarLat, services::location::lat());
  services::location::init();                    // and stays cleared across a reboot
  TEST_ASSERT_EQUAL_DOUBLE(config::kDefaultRadarLat, services::location::lat());
}

// ---------------------------------------------- the debug switch, OFF ------
// test_debug_log covers the enabled expansion; this covers the default one.
// They cannot share a translation unit: debug_log.h is #pragma once and picks
// its expansion at include time. This file includes it WITHOUT defining the
// flag, which is what every production build does.

static void test_debug_logging_is_off_by_default() {
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, DEBUG_LOG_ENABLED,
      "verbose logging must never be on in a normal build");
}

static void test_a_disabled_debug_line_prints_nothing() {
  Serial.capture = true; Serial.log.clear();
  DEBUG_LOG("this must not appear %d", 1);
  DEBUG_LOG_HEAP("nor this");
  char m[192];
  snprintf(m, sizeof(m), "serial log was: '%s'", Serial.log.c_str());
  TEST_ASSERT_TRUE_MESSAGE(Serial.log.empty(), m);
  Serial.capture = false;
}

// The disabled macro must not evaluate its arguments -- a side effect hidden in
// a log call would exist in debug builds and vanish in release ones, which is
// the hardest kind of difference to track down. The header documents this
// rule; this is what enforces it.
static void test_a_disabled_debug_line_does_not_evaluate_its_arguments() {
  int calls = 0;
  auto bump = [&calls]() { return ++calls; };
  DEBUG_LOG("value %d", bump());
  DEBUG_LOG_HEAP(bump() ? "a" : "b");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, calls,
      "arguments must not be evaluated when logging is compiled out");
}

// It has to be usable as a statement anywhere a call would be, including as the
// lone body of an unbraced if -- the do/while(0) form is what guarantees that.
static void test_a_disabled_debug_line_is_a_well_formed_statement() {
  int taken = 0;
  if (DEBUG_LOG_ENABLED) DEBUG_LOG("in the if");
  else taken = 1;
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, taken,
      "DEBUG_LOG must not swallow or detach a following else");

  // A bare `{ }` expansion compiles here but detaches the else -- the heap
  // macro needs the same do/while(0) guarantee, in both expansions.
  taken = 0;
  if (DEBUG_LOG_ENABLED) DEBUG_LOG_HEAP("in the if");
  else taken = 1;
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, taken,
      "DEBUG_LOG_HEAP must not swallow or detach a following else");
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_presets_are_sorted_and_labelled_uniquely);
  RUN_TEST(test_labels_render_expected_text);
  RUN_TEST(test_label_buffer_is_never_overrun);
  RUN_TEST(test_one_tap_advances_exactly_one_preset);
  RUN_TEST(test_tap_cycles_and_wraps);
  RUN_TEST(test_preset_survives_a_reboot);
  RUN_TEST(test_first_boot_with_empty_nvs_uses_defaults);
  RUN_TEST(test_saved_index_out_of_range_falls_back_to_default);
  RUN_TEST(test_fetch_radius_exceeds_the_ring_so_rim_dots_have_data);
  RUN_TEST(test_checkbox_parsing);
  RUN_TEST(test_units_and_runways_are_actually_persisted);
  RUN_TEST(test_reset_restores_both_toggles_and_clears_storage);
  RUN_TEST(test_reset_preserves_the_range_preset);
  RUN_TEST(test_debug_logging_is_off_by_default);
  RUN_TEST(test_a_disabled_debug_line_prints_nothing);
  RUN_TEST(test_a_disabled_debug_line_does_not_evaluate_its_arguments);
  RUN_TEST(test_a_disabled_debug_line_is_a_well_formed_statement);
  RUN_TEST(test_a_refused_nvs_write_still_applies_the_location);
  RUN_TEST(test_invalid_coordinates_are_rejected_and_change_nothing);
  RUN_TEST(test_a_refused_nvs_write_still_applies_the_range_settings);
  RUN_TEST(test_nvs_open_failure_leaves_the_button_working);
  RUN_TEST(test_location_defaults_before_anything_is_saved);
  RUN_TEST(test_location_round_trips_through_nvs);
  RUN_TEST(test_location_rejects_bad_input_and_keeps_the_old_value);
  RUN_TEST(test_location_accepts_the_extremes);
  RUN_TEST(test_snapshot_returns_a_matching_pair);
  RUN_TEST(test_clear_restores_defaults);
  return UNITY_END();
}
