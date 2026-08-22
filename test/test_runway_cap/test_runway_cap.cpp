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

/**
 * Newark: KEWR, KJFK and KLGA all fall inside the widest preset's fetch disc.
 * With the cap at 1 only the first airport gets a strip, so the later ones are
 * fully truncated -- which is the only way to observe whether their labels are
 * still collected. (Centring on a single airport hides the bug: its own first
 * runway is inserted before the cap fills, so it gets labelled either way.)
 */
static constexpr double kLemdLat = 40.6894;
static constexpr double kLemdLon = -74.1705;

void setUp() {
  g_nvs.reset(); g_gfx.resetAll(); mockSetMs(500000); g_font_is_smooth = false;
  Preferences seed; seed.begin("planeradar", false); // Widest preset (25 km ring -> ~36.8 km fetch disc): the radius my
  // multi-airport centre was chosen against.
  seed.putUChar("rangeIdx", 4); seed.end();
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
static void test_a_fully_truncated_airport_keeps_its_label() {
  ui::runway::drawLargeAirportRunways(tft);
  // Only one strip fits, so at least two of these three airports contribute no
  // strip at all. Every one of them must still be identified.
  int labelled = 0;
  for (const char* icao : {"KEWR", "KJFK", "KLGA"}) if (drewLabel(icao)) ++labelled;
  char m[160];
  snprintf(m, sizeof(m),
           "only %d of KEWR/KJFK/KLGA labelled with the strip cap at 1 -- an "
           "airport whose strips are dropped must not lose its identity", labelled);
  TEST_ASSERT_EQUAL_INT_MESSAGE(3, labelled, m);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_cap_really_truncates_here);
  RUN_TEST(test_a_fully_truncated_airport_keeps_its_label);
  return UNITY_END();
}
