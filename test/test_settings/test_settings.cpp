// Real radar_range.cpp and radar_location.cpp against an in-memory NVS.
#include <Arduino.h>
#include <unity.h>
#include <cmath>
#include <cstring>

#include "../mocks/mock_globals.h"
#include "../../src/services/radar_location.cpp"
#include "../../src/ui/radar_range.cpp"

using namespace ui::radar;

void setUp() { g_nvs.reset(); mockSetMs(1000); }
void tearDown() {}

// ---------------- range presets ----------------

static void test_presets_are_sorted_and_labelled_uniquely() {
  TEST_ASSERT_EQUAL_INT(5, (int)kRangePresetCount);
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

static void test_tap_cycles_and_wraps() {
  rangeInit();
  const float first = rangeCurrent().ring3_km;
  for (size_t i = 0; i < kRangePresetCount; ++i) rangeNext();
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(first, rangeCurrent().ring3_km,
                                  "a full cycle must return to the start");
}

static void test_preset_survives_a_reboot() {
  rangeInit(); rangeNext(); rangeNext();
  const float chosen = rangeCurrent().ring3_km;
  rangeInit();                                   // simulate a power cycle
  TEST_ASSERT_EQUAL_FLOAT(chosen, rangeCurrent().ring3_km);
}

// The 20 km preset was inserted mid-array, so a saved index means a different
// distance than it did before. Out-of-range indices must clamp, not crash.
static void test_saved_index_out_of_range_falls_back_to_default() {
  Preferences p; p.begin("planeradar", false); p.putUChar("rangeIdx", 200); p.end();
  rangeInit();
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(10.0f, rangeCurrent().ring3_km,
                                  "a bogus saved index must fall back to the 10 km default");
}

static void test_fetch_radius_exceeds_the_ring_so_rim_dots_have_data() {
  rangeInit();
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

static void test_units_and_runways_round_trip_and_reset() {
  rangeInit();
  saveMilesFromPortal("T");   TEST_ASSERT_TRUE(useMiles());
  saveMilesFromPortal("");    TEST_ASSERT_FALSE(useMiles());
  saveRunwaysFromPortal("");  TEST_ASSERT_FALSE(showRunways());
  saveRunwaysFromPortal("T"); TEST_ASSERT_TRUE(showRunways());
  rangeInit();                                   // persisted?
  TEST_ASSERT_TRUE(showRunways());
  unitsReset();
  TEST_ASSERT_FALSE(useMiles());
  TEST_ASSERT_TRUE_MESSAGE(showRunways(), "runways default back ON after a reset");
}

// A BOOT-hold reset clears units and runways but NOT the range preset.
static void test_reset_preserves_the_range_preset() {
  rangeInit(); rangeNext(); rangeNext();
  const float chosen = rangeCurrent().ring3_km;
  unitsReset();
  rangeInit();
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(chosen, rangeCurrent().ring3_km,
      "documented behaviour: a credential reset leaves the range alone");
}

static void test_nvs_open_failure_is_survivable() {
  g_nvs.open_fail_count = 99;                    // every begin() fails
  rangeInit(); rangeNext(); saveMilesFromPortal("T");
  TEST_ASSERT_TRUE_MESSAGE(rangeCurrent().ring3_km > 0.0f, "must not crash or corrupt state");
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

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_presets_are_sorted_and_labelled_uniquely);
  RUN_TEST(test_labels_render_expected_text);
  RUN_TEST(test_label_buffer_is_never_overrun);
  RUN_TEST(test_tap_cycles_and_wraps);
  RUN_TEST(test_preset_survives_a_reboot);
  RUN_TEST(test_saved_index_out_of_range_falls_back_to_default);
  RUN_TEST(test_fetch_radius_exceeds_the_ring_so_rim_dots_have_data);
  RUN_TEST(test_checkbox_parsing);
  RUN_TEST(test_units_and_runways_round_trip_and_reset);
  RUN_TEST(test_reset_preserves_the_range_preset);
  RUN_TEST(test_nvs_open_failure_is_survivable);
  RUN_TEST(test_location_defaults_before_anything_is_saved);
  RUN_TEST(test_location_round_trips_through_nvs);
  RUN_TEST(test_location_rejects_bad_input_and_keeps_the_old_value);
  RUN_TEST(test_location_accepts_the_extremes);
  RUN_TEST(test_snapshot_returns_a_matching_pair);
  RUN_TEST(test_clear_restores_defaults);
  return UNITY_END();
}
