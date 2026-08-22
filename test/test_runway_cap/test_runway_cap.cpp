// The strip cap is unreachable with the shipped dataset (worst case 12 strips
// against a cap of 32), so the label-before-cap ordering cannot be exercised in
// the normal suite. Here the cap is forced to 1 and the ordering is pinned:
// an airport whose strips are dropped must STILL be identified.
#define RUNWAY_MAX_CACHED_SEGMENTS 1

#include <Arduino.h>
#include <unity.h>
#include <cstring>

#include "../mocks/mock_globals.h"
#include "../../src/services/radar_location.cpp"
#include "../../src/ui/radar_range.cpp"
#include "../../src/ui/radar_geo.cpp"
#include "../../src/hardware/display.cpp"
#include "../../src/data/large_airports_data.cpp"
// The palette globals live in radar_display.cpp, which this suite deliberately
// does not include -- the runway overlay is what is under test here.
namespace ui { namespace radar {
uint16_t kColorBackground = 0x0000;
uint16_t kColorGrid = 0x0320;
uint16_t kColorLabel = 0xFFFF;
uint16_t kColorCenter = 0xFFFF;
uint16_t kColorAircraft = 0x001F;
uint16_t kColorAircraftStale = 0x000A;
uint16_t kColorTrackVector = 0xF81F;
uint16_t kColorTagType = 0xFE00;
uint16_t kColorTagAltitude = 0x5DFF;
uint16_t kColorRunway = 0x4D5F;
uint16_t kColorRunwayLabel = 0x7DFF;
}}

#include "../../src/ui/runway_overlay.cpp"

using namespace ui::radar;

/** Madrid-Barajas: four runways, so the cap of 1 truncates three of them. */
static constexpr double kLemdLat = 40.4719;
static constexpr double kLemdLon = -3.5626;

void setUp() {
  g_nvs.reset(); g_gfx.resetAll(); mockSetMs(500000); g_font_is_smooth = false;
  Preferences seed; seed.begin("planeradar", false); seed.putUChar("rangeIdx", 1); seed.end();
  rangeInit();
  char a[32], b[32];
  snprintf(a, sizeof(a), "%.6f", kLemdLat);
  snprintf(b, sizeof(b), "%.6f", kLemdLon);
  services::location::saveFromStrings(a, b);
  saveRunwaysFromPortal("T");
}
void tearDown() {}

static int strips() {
  int n = 0;
  for (const auto& o : g_gfx.of(DrawOp::WideLine)) if (o.color == kColorRunway) ++n;
  return n;
}
static bool drewLabel(const char* icao) {
  for (const auto& o : g_gfx.of(DrawOp::Text)) if (o.text == icao) return true;
  return false;
}

static void test_the_cap_really_truncates_here() {
  ui::runway::drawLargeAirportRunways(tft);
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, strips(),
      "with the cap at 1 exactly one strip may be cached -- if this is not 1 "
      "the override did not take effect and the ordering test below is vacuous");
}

// THE BUG: label collection used to sit AFTER the cap check, so once the cache
// filled, later airports lost their strips AND their identity. Moving it before
// the check is the fix; this is what pins it.
static void test_a_truncated_airport_keeps_its_label() {
  ui::runway::drawLargeAirportRunways(tft);
  TEST_ASSERT_TRUE_MESSAGE(drewLabel("LEMD"),
      "LEMD has four strips and the cap is 1, so three are dropped -- its ICAO "
      "label must still be drawn, or a truncated airport becomes anonymous");
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_cap_really_truncates_here);
  RUN_TEST(test_a_truncated_airport_keeps_its_label);
  return UNITY_END();
}
