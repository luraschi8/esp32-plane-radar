// Exercises the real radar_display.cpp and runway_overlay.cpp against a
// recording canvas: what was drawn, where, in what colour, in what order.
// These two files produced most of this project's shipped bugs and had no
// coverage at all.
#include <Arduino.h>
#include <unity.h>
#include <cmath>
#include <cstring>
#include <string>

#include "../mocks/mock_globals.h"
#include "../../src/services/radar_location.cpp"
#include "../../src/ui/radar_range.cpp"
#include "../../src/ui/radar_geo.cpp"
#include "../../src/hardware/display.cpp"
#include "../../src/data/large_airports_data.cpp"
#include "../../src/ui/runway_overlay.cpp"
#include "../../src/ui/radar_display.cpp"
#include "../../src/services/adsb_client.cpp"

using namespace ui;
using namespace ui::radar;

static constexpr double kLat = 40.445564;
static constexpr double kLon = -3.698361;

/** Build a payload placing aircraft at chosen offsets in km from the centre. */
struct Target {
  float east_km, north_km, gs_kt, track_deg, seen_pos;
  const char* callsign;
};
static std::string payloadFor(const Target* t, int n) {
  const double cos_lat = cos(kLat * M_PI / 180.0);
  std::string s = "{\"ac\":[";
  for (int i = 0; i < n; ++i) {
    const double lat = kLat + t[i].north_km / 111.0;
    const double lon = kLon + t[i].east_km / (111.0 * cos_lat);
    char b[320];
    snprintf(b, sizeof(b),
             "%s{\"hex\":\"h%d\",\"flight\":\"%s\",\"t\":\"B738\",\"lat\":%.6f,"
             "\"lon\":%.6f,\"alt_baro\":3000,\"gs\":%.1f,\"track\":%.1f,"
             "\"true_heading\":%.1f,\"seen_pos\":%.2f}",
             i ? "," : "", i, t[i].callsign, lat, lon, t[i].gs_kt,
             t[i].track_deg, t[i].track_deg, t[i].seen_pos);
    s += b;
  }
  return s + "]}";
}
static void publishTargets(const Target* t, int n) {
  const std::string p = payloadFor(t, n);
  g_http.reset(); g_http.body = p; g_http.code = HTTP_CODE_OK;
  TEST_ASSERT_TRUE(services::adsb::fetchUpdate(kLat, kLon, 30.0f));
}

/**
 * Font metrics are computed once and latched in file-statics, so the smooth-font
 * path is unreachable after any test has run the bitmap path. These live in
 * anonymous namespaces inside the included .cpp, so this TU can clear them.
 */
static void useFont(bool smooth) {
  g_font_is_smooth = smooth;
  ui::s_label_metrics_ready = false;
  ui::s_tag_label_metrics_ready = false;
  ui::runway::s_runway_label_ready = false;
}

void setUp() {
  g_nvs.reset(); g_gfx.resetAll(); mockSetMs(500000); g_mutex_take_fails = 0; useFont(false);
  Preferences seed; seed.begin("planeradar", false); seed.putUChar("rangeIdx", 1); seed.end();
  rangeInit();
  services::location::saveFromStrings("40.445564", "-3.698361");
}
void tearDown() {}

// ---------------------------------------------------------------- tags -----

struct Rect { int x, y, w, h; };

/**
 * Resolve a recorded text op to its actual pixel box. The datum decides which
 * corner x/y refer to; treating every op as top-left silently mislocates
 * anything drawn with a centre or right datum (the scale label, the cardinals,
 * the runway ICAOs).
 */
static Rect textRect(const DrawOp& o) {
  int left = o.x, top = o.y;
  switch (o.datum) {
    case top_center: case middle_center: case bottom_center: left = o.x - o.w / 2; break;
    case top_right:  case middle_right:  case bottom_right:  left = o.x - o.w;     break;
    default: break;
  }
  switch (o.datum) {
    case middle_left: case middle_center: case middle_right: top = o.y - o.h / 2; break;
    case bottom_left: case bottom_center: case bottom_right: top = o.y - o.h;     break;
    default: break;
  }
  return {left, top, o.w, o.h};
}
static bool overlaps(const Rect& a, const Rect& b) {
  return !(a.x + a.w <= b.x || b.x + b.w <= a.x || a.y + a.h <= b.y || b.y + b.h <= a.y);
}
/**
 * Aircraft tag lines only. The recorded text also contains cardinal letters,
 * the range label, and runway ICAOs -- and a runway ICAO is deliberately drawn
 * three times at 1 px offsets to fake bold, so counting those as tags makes the
 * overlap assertion fail against correct code.
 */
static bool isTagText(const std::string& t) {
  return t.rfind("AAA", 0) == 0 || t.rfind("BBB", 0) == 0 || t.rfind("CCC", 0) == 0 ||
         t.rfind("DDD", 0) == 0 || t.rfind("NEAR", 0) == 0 || t.rfind("MID", 0) == 0 ||
         t.rfind("FAR", 0) == 0 || t.rfind("MOVER", 0) == 0 || t.rfind("OLDFIX", 0) == 0 ||
         t.rfind("FRESH", 0) == 0 || t.rfind("STALE", 0) == 0 ||
         t == "B738" || t == "3000 ft";
}
static std::vector<Rect> tagBlocks() {
  std::vector<Rect> out;
  for (const auto& o : g_gfx.of(DrawOp::Text)) {
    if (o.text.empty() || !isTagText(o.text)) continue;
    out.push_back(textRect(o));
  }
  return out;
}

// THE OVERPRINT BUG: nearby traffic drew three-line blocks on top of each other.
static void test_tags_never_overlap_each_other() {
  saveRunwaysFromPortal("");   // isolate the traffic layer
  Target t[] = {{2.0f, 0.2f, 200, 90, 0.1f, "AAA111"},
                {2.0f, 0.0f, 200, 90, 0.1f, "BBB222"},
                {2.1f, -0.2f, 200, 90, 0.1f, "CCC333"},
                {2.0f, -0.4f, 200, 90, 0.1f, "DDD444"}};
  publishTargets(t, 4);
  radarDisplayDraw();
  const auto blocks = tagBlocks();
  TEST_ASSERT_TRUE_MESSAGE(blocks.size() >= 6,
      "precondition: tags must actually be drawn, or this passes vacuously");
  for (size_t i = 0; i < blocks.size(); ++i)
    for (size_t j = i + 1; j < blocks.size(); ++j) {
      char m[128];
      snprintf(m, sizeof(m), "tag %zu (%d,%d %dx%d) overlaps tag %zu (%d,%d %dx%d)",
               i, blocks[i].x, blocks[i].y, blocks[i].w, blocks[i].h,
               j, blocks[j].x, blocks[j].y, blocks[j].w, blocks[j].h);
      TEST_ASSERT_FALSE_MESSAGE(overlaps(blocks[i], blocks[j]), m);
    }
}

// THE DISPLACEMENT BUG: tags were nudged by whole blocks (~51 px), far enough
// from their symbol to read as belonging to a different aircraft.
static void test_a_tag_stays_next_to_its_own_symbol() {
  saveRunwaysFromPortal("");   // isolate the traffic layer
  Target t[] = {{2.0f, 0.2f, 200, 90, 0.1f, "AAA111"},
                {2.0f, 0.0f, 200, 90, 0.1f, "BBB222"},
                {2.0f, -0.2f, 200, 90, 0.1f, "CCC333"}};
  publishTargets(t, 3);
  radarDisplayDraw();
  // Every triangle is a symbol; every tag block must sit near one of them.
  const auto tris = g_gfx.of(DrawOp::Triangle);
  TEST_ASSERT_TRUE(tris.size() >= 3);
  // Measure from the tag's NEAR edge to the symbol, not from its centre, and
  // do not add the block width -- doing both made the effective gate ~100 px on
  // a 240 px panel, wide enough to accept the ~51 px displacement bug this test
  // exists to catch.
  TEST_ASSERT_TRUE_MESSAGE(tagBlocks().size() >= 3,
      "precondition: tags must actually be drawn, or this passes vacuously");
  const int max_dy = g_gfx.line_height * 2;
  const int max_dx = 34;   // symbol half-width + gap + a little slack
  for (const auto& b : tagBlocks()) {
    bool near_a_symbol = false;
    for (const auto& tri : tris) {
      const int near_edge = (b.x + b.w / 2 < tri.x) ? (b.x + b.w) : b.x;
      const int cy = b.y + b.h / 2;
      if (abs(near_edge - tri.x) <= max_dx && abs(cy - tri.y) <= max_dy + b.h)
        near_a_symbol = true;
    }
    char m[96];
    snprintf(m, sizeof(m), "tag at (%d,%d) is not adjacent to any symbol", b.x, b.y);
    TEST_ASSERT_TRUE_MESSAGE(near_a_symbol, m);
  }
}

// When space runs out the NEAREST aircraft keeps its label.
static void test_the_nearest_aircraft_keeps_its_label() {
  saveRunwaysFromPortal("");   // isolate the traffic layer
  Target t[] = {{1.0f, 0.0f, 200, 90, 0.1f, "NEAR11"},
                {1.05f, 0.05f, 200, 90, 0.1f, "MID222"},
                {1.1f, 0.1f, 200, 90, 0.1f, "FAR333"},
                {1.15f, 0.15f, 200, 90, 0.1f, "FAR444"},
                {1.2f, 0.2f, 200, 90, 0.1f, "FAR555"}};
  publishTargets(t, 5);
  radarDisplayDraw();
  bool near_labelled = false;
  for (const auto& o : g_gfx.of(DrawOp::Text)) if (o.text == "NEAR11") near_labelled = true;
  TEST_ASSERT_TRUE_MESSAGE(near_labelled,
      "with tags contending, the closest target must be the one that keeps its label");
  // and every symbol is still drawn even when its label was dropped
  TEST_ASSERT_EQUAL_INT_MESSAGE(5, (int)g_gfx.count(DrawOp::Triangle),
      "a dropped label must not drop the aircraft");
}

// ------------------------------------------------------- dead reckoning -----

static int symbolX(const char* callsign_unused) {
  const auto tris = g_gfx.of(DrawOp::Triangle);
  return tris.empty() ? -1 : tris[0].x;
}

static void test_target_advances_along_its_track_between_fetches() {
  Target t[] = {{0.0f, 0.0f, 400, 90, 0.0f, "MOVER1"}};   // due east, fast
  publishTargets(t, 1);
  radarDisplayDraw();
  const int x0 = symbolX("MOVER1");
  g_gfx.reset();
  mockAdvanceMs(6000);                                     // 6 s later
  radarDisplayDraw();
  const int x1 = symbolX("MOVER1");
  TEST_ASSERT_TRUE_MESSAGE(x1 > x0,
      "an eastbound target must move right as dead reckoning advances it");
}

// The clamp must cap seen_pos + our own age, not just our own age.
static void test_extrapolation_is_clamped_on_the_total_age() {
  Target near_horizon[] = {{0.0f, 0.0f, 400, 90, 11.0f, "OLDFIX"}};
  publishTargets(near_horizon, 1);
  radarDisplayDraw();
  const int x_at_11 = symbolX("OLDFIX");
  g_gfx.reset();
  mockAdvanceMs(8000);      // total age 19 s, well past the 12 s horizon
  radarDisplayDraw();
  const int x_later = symbolX("OLDFIX");
  // 11 s of the horizon was already consumed by seen_pos, so at most 1 s more.
  const float px_per_km = pxPerKm();
  const int one_second_px = (int)(400.0f * 1.852f / 3600.0f * px_per_km) + 2;
  char m[128];
  snprintf(m, sizeof(m), "moved %d px after the clamp; at most %d expected",
           x_later - x_at_11, one_second_px);
  TEST_ASSERT_TRUE_MESSAGE(x_later - x_at_11 <= one_second_px, m);
}

static void test_a_stale_fix_is_dimmed_not_hidden() {
  Target fresh[] = {{2.0f, 0.0f, 200, 90, 0.1f, "FRESH1"}};
  publishTargets(fresh, 1);
  radarDisplayDraw();
  const uint16_t live_colour = g_gfx.of(DrawOp::Triangle)[0].color;

  g_gfx.reset();
  Target stale[] = {{2.0f, 0.0f, 200, 90, 30.0f, "STALE1"}};   // fix 30 s old
  publishTargets(stale, 1);
  radarDisplayDraw();
  const auto tris = g_gfx.of(DrawOp::Triangle);
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, (int)tris.size(), "a stale target is still shown");
  TEST_ASSERT_TRUE_MESSAGE(tris[0].color != live_colour,
      "a fix older than the horizon must be visually distinguishable");
}

// A stalled feed must dim EVERYTHING, then clear it entirely at expiry.
static void test_a_stalled_feed_dims_then_clears() {
  Target t[] = {{2.0f, 0.0f, 200, 90, 0.1f, "AAA111"}};
  publishTargets(t, 1);
  radarDisplayDraw();
  const uint16_t live = g_gfx.of(DrawOp::Triangle)[0].color;

  g_gfx.reset(); mockAdvanceMs(20000);          // 20 s: past the DR horizon
  radarDisplayDraw();
  TEST_ASSERT_EQUAL_INT(1, (int)g_gfx.count(DrawOp::Triangle));
  TEST_ASSERT_TRUE_MESSAGE(g_gfx.of(DrawOp::Triangle)[0].color != live,
      "a stalled feed must dim every target, not present them as live");

  g_gfx.reset(); mockAdvanceMs(50000);          // 70 s total: expired
  radarDisplayDraw();
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)g_gfx.count(DrawOp::Triangle),
      "past the expiry window the traffic layer must be gone entirely");
  TEST_ASSERT_TRUE_MESSAGE(g_gfx.count(DrawOp::Circle) > 0,
      "...but the grid must still be drawn");
}

// ------------------------------------------------------------- frame -------

static void test_a_normal_frame_is_blitted_once() {
  Target t[] = {{2.0f, 0.0f, 200, 90, 0.1f, "AAA111"}};
  publishTargets(t, 1);
  TEST_ASSERT_TRUE(radarDisplayDraw());
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, (int)g_gfx.count(DrawOp::Push),
      "the composed frame must reach the panel exactly once");
}

static void test_sprite_failure_falls_back_to_direct_drawing() {
  g_gfx.sprite_alloc_fails = true;
  Target t[] = {{2.0f, 0.0f, 200, 90, 0.1f, "AAA111"}};
  publishTargets(t, 1);
  TEST_ASSERT_TRUE_MESSAGE(radarDisplayDraw(),
      "without a sprite the panel is written directly, so the frame IS shown");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)g_gfx.count(DrawOp::Push), "nothing to blit");
  TEST_ASSERT_TRUE_MESSAGE(g_gfx.count(DrawOp::Triangle) > 0, "traffic still drawn");
}

// A 115 KB allocation must not be retried every frame on a starved heap.
static void test_sprite_allocation_is_not_retried_every_frame() {
  // Advance past any backoff a previous test may have armed, or this measures
  // inherited state instead of its own.
  mockAdvanceMs(60000);
  g_gfx.sprite_alloc_fails = true;
  Target t[] = {{2.0f, 0.0f, 200, 90, 0.1f, "AAA111"}};
  publishTargets(t, 1);
  radarDisplayDraw();
  const int after_first = g_gfx.sprite_alloc_attempts;
  TEST_ASSERT_TRUE_MESSAGE(after_first > 0,
      "this test must itself trigger an allocation attempt, not inherit one");
  for (int i = 0; i < 20; ++i) { mockAdvanceMs(100); radarDisplayRefreshAircraft(); }
  TEST_ASSERT_EQUAL_INT_MESSAGE(after_first, g_gfx.sprite_alloc_attempts,
      "inside the backoff window the allocation must not be attempted again");
  mockAdvanceMs(6000);
  radarDisplayRefreshAircraft();
  TEST_ASSERT_TRUE_MESSAGE(g_gfx.sprite_alloc_attempts > after_first,
      "after the backoff it should try once more");
}

// --------------------------------------------------------------- grid ------

// The crosshairs cost 25.9 ms/frame as anti-aliased wide lines; they are
// axis-aligned so they must be plain rectangles.
static void test_crosshairs_are_rectangles_not_antialiased_lines() {
  Target none[] = {{2.0f, 0.0f, 200, 90, 0.1f, "AAA111"}};
  publishTargets(none, 1);
  radarDisplayDraw();
  int full_span_wide_lines = 0;
  for (const auto& o : g_gfx.of(DrawOp::WideLine))
    if (abs(o.x - o.x2) > 150 || abs(o.y - o.y2) > 150) ++full_span_wide_lines;
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, full_span_wide_lines,
      "a full-diameter drawWideLine is the 25.9 ms crosshair regression");
  bool h = false, v = false;
  for (const auto& o : g_gfx.of(DrawOp::FillRect)) {
    if (o.w > 200 && o.h <= 4) h = true;
    if (o.h > 200 && o.w <= 4) v = true;
  }
  TEST_ASSERT_TRUE_MESSAGE(h && v, "both spokes must be drawn as fillRect");
}

static void test_grid_has_the_expected_rings() {
  Target t[] = {{2.0f, 0.0f, 200, 90, 0.1f, "AAA111"}};
  publishTargets(t, 1);
  radarDisplayDraw();
  int outer = 0;
  for (const auto& o : g_gfx.of(DrawOp::Circle)) if (o.r == kGridOuterRadius) ++outer;
  TEST_ASSERT_TRUE_MESSAGE(outer > 0, "the outer ring must be drawn at kGridOuterRadius");
  TEST_ASSERT_TRUE_MESSAGE(g_gfx.count(DrawOp::Circle) >= (size_t)kRingCount,
                           "every ring must be drawn");
}

// ------------------------------------------------------- runway overlay ----

/** Madrid-Barajas: four runways, and the airport this device actually sees. */
static constexpr double kLemdLat = 40.4719;
static constexpr double kLemdLon = -3.5626;

static void atAirport() {
  char a[32], b[32];
  snprintf(a, sizeof(a), "%.6f", kLemdLat);
  snprintf(b, sizeof(b), "%.6f", kLemdLon);
  services::location::saveFromStrings(a, b);
  saveRunwaysFromPortal("T");
}
/** Runway strips are the teal wide lines; aircraft vectors are magenta. */
static int runwayStrips() {
  int n = 0;
  for (const auto& o : g_gfx.of(DrawOp::WideLine))
    if (o.color == kColorRunway) ++n;
  return n;
}
static bool drewLabel(const char* icao) {
  for (const auto& o : g_gfx.of(DrawOp::Text)) if (o.text == icao) return true;
  return false;
}

static void test_runways_are_drawn_at_a_real_airport() {
  atAirport();
  radarDisplayDraw();
  TEST_ASSERT_TRUE_MESSAGE(runwayStrips() > 0, "LEMD's strips must be drawn");
  TEST_ASSERT_TRUE_MESSAGE(drewLabel("LEMD"), "the ICAO label must be drawn");
}

static void test_no_runways_when_the_overlay_is_switched_off() {
  atAirport();
  saveRunwaysFromPortal("");
  radarDisplayDraw();
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, runwayStrips(), "the toggle must suppress the overlay");
  TEST_ASSERT_FALSE_MESSAGE(drewLabel("LEMD"), "and its labels");
}

static void test_no_runways_in_the_middle_of_the_ocean() {
  services::location::saveFromStrings("0.0", "-140.0");   // South Pacific
  saveRunwaysFromPortal("T");
  radarDisplayDraw();
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, runwayStrips(), "no airport is anywhere near");
}

// The cache exists because rescanning 1706 segments per frame cost ~34 ms. It
// must survive repeated frames unchanged, and must rebuild when the view moves.
static void test_cached_geometry_is_stable_across_frames() {
  atAirport();
  radarDisplayDraw();
  std::vector<DrawOp> first = g_gfx.of(DrawOp::WideLine);
  g_gfx.reset();
  radarDisplayDraw();
  std::vector<DrawOp> second = g_gfx.of(DrawOp::WideLine);
  TEST_ASSERT_EQUAL_INT_MESSAGE((int)first.size(), (int)second.size(),
                                "the same view must draw the same strips");
  for (size_t i = 0; i < first.size(); ++i) {
    TEST_ASSERT_EQUAL_INT(first[i].x, second[i].x);
    TEST_ASSERT_EQUAL_INT(first[i].y, second[i].y);
  }
}

static void test_cache_rebuilds_when_the_range_changes() {
  atAirport();
  radarDisplayDraw();
  int before_x = -1;
  for (const auto& o : g_gfx.of(DrawOp::WideLine)) if (o.color == kColorRunway) { before_x = o.x; break; }
  TEST_ASSERT_TRUE(before_x >= 0);
  g_gfx.reset();
  rangeNext();                       // a different scale: geometry must move
  radarDisplayDraw();
  int after_x = -1;
  for (const auto& o : g_gfx.of(DrawOp::WideLine)) if (o.color == kColorRunway) { after_x = o.x; break; }
  TEST_ASSERT_TRUE(after_x >= 0);
  TEST_ASSERT_TRUE_MESSAGE(before_x != after_x,
      "a stale cache would keep drawing the previous preset's geometry");
}

static void test_cache_rebuilds_when_the_location_changes() {
  atAirport();
  radarDisplayDraw();
  const int strips_at_airport = runwayStrips();
  TEST_ASSERT_TRUE(strips_at_airport > 0);
  g_gfx.reset();
  services::location::saveFromStrings("0.0", "-140.0");
  radarDisplayDraw();
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, runwayStrips(),
      "moving the radar centre must invalidate the cached screen geometry");
}

// Strips are capped; labels must still be collected for every in-range airport
// so a dropped strip never costs an airport its identity.
// The caps are only safe because no point on Earth has that many large-airport
// strips within the widest preset's fetch disc. Scan the shipped dataset and
// prove it, rather than assuming: regenerating the table with medium airports
// would silently start truncating.
static void test_the_shipped_dataset_never_reaches_the_caps() {
  using namespace data::large_airports;
  int runways_per_airport[kAirportCount] = {0};
  for (size_t i = 0; i < kRunwayCount; ++i) {
    TEST_ASSERT_TRUE_MESSAGE(kRunways[i].airport_idx < kAirportCount,
        "a runway indexes an airport that does not exist -- out-of-bounds read");
    ++runways_per_airport[kRunways[i].airport_idx];
  }
  // Derived from the preset table, not hardcoded: adding a wider preset must
  // widen this scan too, or the test that exists to catch silent truncation
  // would itself go quietly out of date.
  float radius_km = 0.0f;
  for (size_t i = 0; i < kRangePresetCount; ++i) {
    Preferences sp; sp.begin("planeradar", false); sp.putUChar("rangeIdx", (uint8_t)i); sp.end();
    rangeInit();
    if (fetchRadiusKm() > radius_km) radius_km = fetchRadiusKm();
  }
  TEST_ASSERT_TRUE_MESSAGE(radius_km > 30.0f, "sanity: widest fetch disc should exceed 30 km");
  int worst_strips = 0, worst_airports = 0;
  for (size_t c = 0; c < kAirportCount; ++c) {
    const float clat = kAirports[c].lat_e7 * 1e-7f;
    const float clon = kAirports[c].lon_e7 * 1e-7f;
    const float cosl = cosf(clat * 0.01745329252f);
    int strips = 0, airports = 0;
    for (size_t j = 0; j < kAirportCount; ++j) {
      const float dy = (kAirports[j].lat_e7 * 1e-7f - clat) * 111.0f;
      const float dx = (kAirports[j].lon_e7 * 1e-7f - clon) * 111.0f * cosl;
      if (dx * dx + dy * dy <= radius_km * radius_km) {
        ++airports; strips += runways_per_airport[j];
      }
    }
    if (strips > worst_strips) worst_strips = strips;
    if (airports > worst_airports) worst_airports = airports;
  }
  char m[160];
  snprintf(m, sizeof(m), "worst case %d strips vs cap %d -- truncation is now reachable",
           worst_strips, (int)ui::runway::kMaxCachedSegments);
  TEST_ASSERT_TRUE_MESSAGE(worst_strips <= (int)ui::runway::kMaxCachedSegments, m);
  snprintf(m, sizeof(m), "worst case %d airports vs label cap %d",
           worst_airports, (int)ui::runway::kMaxAirportLabels);
  TEST_ASSERT_TRUE_MESSAGE(worst_airports <= (int)ui::runway::kMaxAirportLabels, m);
}

// Sanity only: an in-range airport is both drawn and identified. The cap is
// unreachable with the shipped dataset, so the label-before-cap ORDERING is
// pinned in test_runway_cap, which forces the cap to 1.
static void test_an_in_range_airport_is_drawn_and_identified() {
  atAirport();
  radarDisplayDraw();
  TEST_ASSERT_TRUE_MESSAGE(runwayStrips() > 0 && drewLabel("LEMD"),
      "an in-range airport must be both drawn and identified");
}

// ------------------------------- silently-deletable draw paths -------------
// Each of the following could be removed entirely without any test failing.

static void test_speed_vectors_are_drawn_for_moving_traffic() {
  saveRunwaysFromPortal("");
  Target t[] = {{2.0f, 0.0f, 300, 90, 0.1f, "AAA111"}};
  publishTargets(t, 1);
  radarDisplayDraw();
  int vectors = 0;
  for (const auto& o : g_gfx.of(DrawOp::WideLine))
    if (o.color == kColorTrackVector) ++vectors;
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, vectors, "a moving target must show its track vector");
}

static void test_a_stationary_target_has_no_speed_vector() {
  saveRunwaysFromPortal("");
  Target t[] = {{2.0f, 0.0f, 0, 90, 0.1f, "AAA111"}};   // gs = 0
  publishTargets(t, 1);
  radarDisplayDraw();
  for (const auto& o : g_gfx.of(DrawOp::WideLine))
    TEST_ASSERT_TRUE_MESSAGE(o.color != kColorTrackVector,
        "a stationary target must not draw a vector");
}

// Traffic outside the ring is shown as a bearing cue on the screen rim.
static void test_beyond_ring_traffic_becomes_a_rim_dot() {
  saveRunwaysFromPortal("");
  Target t[] = {{60.0f, 60.0f, 300, 45, 0.1f, "FARNE1"}};   // NE, far outside
  publishTargets(t, 1);
  radarDisplayDraw();
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)g_gfx.count(DrawOp::Triangle),
      "a target past the ring is not drawn as a symbol");
  int dots = 0, dx = 0, dy = 0;
  for (const auto& o : g_gfx.of(DrawOp::SmoothCircle)) {
    if (o.r != kBeyondRingDotRadiusPx) continue;   // skip the centre dot
    ++dots; dx = o.x - kCenterX; dy = o.y - kCenterY;
  }
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, dots, "exactly one rim dot for one distant target");
  TEST_ASSERT_TRUE_MESSAGE(dx > 0 && dy < 0, "a NE target must sit NE on the rim");
  const int r = (int)sqrtf((float)(dx * dx + dy * dy));
  TEST_ASSERT_INT_WITHIN_MESSAGE(2, kCenterX - kBeyondRingScreenMarginPx, r,
      "the dot must sit on the screen rim, not at the target's true distance");
}

static void test_tag_lines_carry_their_distinct_colours() {
  saveRunwaysFromPortal("");
  Target t[] = {{2.0f, 0.0f, 200, 90, 0.1f, "AAA111"}};
  publishTargets(t, 1);
  radarDisplayDraw();
  bool callsign = false, type = false, alt = false;
  for (const auto& o : g_gfx.of(DrawOp::Text)) {
    if (o.text == "AAA111" && o.color == kColorLabel) callsign = true;
    if (o.text == "B738"   && o.color == kColorTagType) type = true;
    if (o.text == "3000 ft" && o.color == kColorTagAltitude) alt = true;
  }
  TEST_ASSERT_TRUE_MESSAGE(callsign, "callsign line missing or wrong colour");
  TEST_ASSERT_TRUE_MESSAGE(type, "type line missing or wrong colour");
  TEST_ASSERT_TRUE_MESSAGE(alt, "altitude line missing or wrong colour");
}

static void test_crosshairs_are_centred_on_the_radar_origin() {
  Target t[] = {{2.0f, 0.0f, 200, 90, 0.1f, "AAA111"}};
  publishTargets(t, 1);
  radarDisplayDraw();
  bool v = false, h = false;
  for (const auto& o : g_gfx.of(DrawOp::FillRect)) {
    if (o.h > 200 && o.w <= 4 && abs(o.x - kCenterX) <= 1) v = true;
    if (o.w > 200 && o.h <= 4 && abs(o.y - kCenterY) <= 1) h = true;
  }
  TEST_ASSERT_TRUE_MESSAGE(v, "the vertical spoke must pass through the centre");
  TEST_ASSERT_TRUE_MESSAGE(h, "the horizontal spoke must pass through the centre");
}

static void test_cardinal_labels_are_in_the_right_places() {
  Target t[] = {{2.0f, 0.0f, 200, 90, 0.1f, "AAA111"}};
  publishTargets(t, 1);
  radarDisplayDraw();
  int ny = 999, sy = -999, wx = 999, ex = -999;
  for (const auto& o : g_gfx.of(DrawOp::Text)) {
    if (o.text == "N") ny = o.y;
    if (o.text == "S") sy = o.y;
    if (o.text == "W") wx = o.x;
    if (o.text == "E") ex = o.x;
  }
  TEST_ASSERT_TRUE_MESSAGE(ny < kCenterY, "N must be in the upper half");
  TEST_ASSERT_TRUE_MESSAGE(sy > kCenterY, "S must be in the lower half");
  TEST_ASSERT_TRUE_MESSAGE(wx < kCenterX, "W must be on the left");
  TEST_ASSERT_TRUE_MESSAGE(ex > kCenterX, "E must be on the right");
}

static void test_the_scale_label_follows_the_range_and_units() {
  Target t[] = {{2.0f, 0.0f, 200, 90, 0.1f, "AAA111"}};
  publishTargets(t, 1);
  radarDisplayDraw();
  bool km = false;
  for (const auto& o : g_gfx.of(DrawOp::Text)) if (o.text == "10km") km = true;
  TEST_ASSERT_TRUE_MESSAGE(km, "the 10 km preset must label ring 3 as 10km");
  g_gfx.reset();
  saveMilesFromPortal("T");
  radarDisplayDraw();
  bool mi = false;
  for (const auto& o : g_gfx.of(DrawOp::Text)) if (o.text == "6mi") mi = true;
  TEST_ASSERT_TRUE_MESSAGE(mi, "with miles selected the same ring reads 6mi");
  saveMilesFromPortal("");
}

// The producer of blitted==false: if the aircraft list cannot be locked, the
// frame must NOT be pushed, or every target flashes off for a frame.
static void test_a_locked_aircraft_list_suppresses_the_blit() {
  Target t[] = {{2.0f, 0.0f, 200, 90, 0.1f, "AAA111"}};
  publishTargets(t, 1);
  // aircraftLock() short-circuits to true when no mutex exists, so the lock
  // path is only reachable once the fetch task has been started.
  TEST_ASSERT_TRUE_MESSAGE(services::adsb::startFetchTask(),
                           "need a real mutex for the lock to be able to fail");
  g_gfx.reset();
  g_mutex_take_fails = 1;
  const bool blitted = radarDisplayRefreshAircraft();
  TEST_ASSERT_FALSE_MESSAGE(blitted, "a frame missing its traffic must report not-drawn");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)g_gfx.count(DrawOp::Push),
      "pushing it would flash every target off for a frame");
}

static void test_the_grid_is_cleared_every_frame() {
  Target t[] = {{2.0f, 0.0f, 200, 90, 0.1f, "AAA111"}};
  publishTargets(t, 1);
  radarDisplayDraw();
  TEST_ASSERT_TRUE_MESSAGE(g_gfx.count(DrawOp::FillScreen) >= 1,
      "without a clear, the previous frame's traffic ghosts");
}

// ------------------------------- smooth (VLW) font path --------------------
// The device ships the VLW font as its PRIMARY path; the bitmap font is only a
// fallback for when the embedded blob fails to load. Everything above runs the
// fallback, so these re-run the layout invariants with smooth metrics selected.

static void test_smooth_font_is_actually_selected() {
  useFont(true);
  Target t[] = {{2.0f, 0.0f, 200, 90, 0.1f, "AAA111"}};
  publishTargets(t, 1);
  radarDisplayDraw();
  TEST_ASSERT_TRUE_MESSAGE(ui::s_cardinal_use_vlw,
      "with a smooth font available the cardinal labels must use it");
  TEST_ASSERT_TRUE_MESSAGE(ui::s_tag_use_vlw, "and so must the aircraft tags");
}

// findVlwSizeForHeight binary-searches a scale to hit a target cap height.
static void test_the_vlw_size_search_converges_on_the_target_height() {
  useFont(true);
  Target t[] = {{2.0f, 0.0f, 200, 90, 0.1f, "AAA111"}};
  publishTargets(t, 1);
  radarDisplayDraw();
  TEST_ASSERT_TRUE_MESSAGE(ui::s_cardinal_vlw_size > 0.25f && ui::s_cardinal_vlw_size < 1.2f,
      "the search must land inside its own bracket, not on an endpoint");
  // Applying that size must reproduce roughly the requested cap height.
  displayFontSetSmoothSize(tft, ui::s_cardinal_vlw_size);
  TEST_ASSERT_INT_WITHIN_MESSAGE(3, kCardinalLabelHeightPx, tft.fontHeight(),
      "the chosen scale must actually produce the target height");
}

static void test_the_scale_label_is_smaller_than_the_cardinals() {
  useFont(true);
  Target t[] = {{2.0f, 0.0f, 200, 90, 0.1f, "AAA111"}};
  publishTargets(t, 1);
  radarDisplayDraw();
  TEST_ASSERT_TRUE_MESSAGE(ui::s_scale_vlw_size < ui::s_cardinal_vlw_size,
      "the range label is deliberately set shorter than N/S/E/W");
}

static void test_tags_never_overlap_with_the_smooth_font_either() {
  useFont(true);
  saveRunwaysFromPortal("");
  Target t[] = {{2.0f, 0.2f, 200, 90, 0.1f, "AAA111"},
                {2.0f, 0.0f, 200, 90, 0.1f, "BBB222"},
                {2.1f, -0.2f, 200, 90, 0.1f, "CCC333"},
                {2.0f, -0.4f, 200, 90, 0.1f, "DDD444"}};
  publishTargets(t, 4);
  radarDisplayDraw();
  const auto blocks = tagBlocks();
  TEST_ASSERT_TRUE_MESSAGE(blocks.size() >= 6, "precondition: tags must be drawn");
  for (size_t i = 0; i < blocks.size(); ++i)
    for (size_t j = i + 1; j < blocks.size(); ++j)
      TEST_ASSERT_FALSE_MESSAGE(overlaps(blocks[i], blocks[j]),
          "collision avoidance must hold for the smooth font too");
}

static void test_runway_labels_render_with_the_smooth_font() {
  useFont(true);
  atAirport();
  radarDisplayDraw();
  TEST_ASSERT_TRUE_MESSAGE(ui::runway::s_runway_label_use_vlw,
      "runway ICAOs must use the smooth font when it is available");
  TEST_ASSERT_TRUE_MESSAGE(drewLabel("LEMD"), "and still be drawn");
}

// --------------------------------- nothing may be drawn off-panel ----------

static void test_no_text_or_rect_is_drawn_outside_the_panel() {
  atAirport();
  Target t[] = {{2.0f, 1.5f, 200, 90, 0.1f, "WWMM88"},    // wide glyphs, near rim
                {-2.0f, -1.5f, 200, 270, 0.1f, "IIll11"}};
  publishTargets(t, 2);
  radarDisplayDraw();
  for (const auto& o : g_gfx.of(DrawOp::Text)) {
    // The N/S/E/W bezel labels are deliberately nudged past the edge
    // (kCardinalNorthOffsetY = -1 etc.) to sit against the rim of the round
    // panel, so they are exempt. Everything else must be fully visible.
    if (o.text.size() == 1 && strchr("NSEW", o.text[0])) continue;
    const Rect r = textRect(o);
    char m[176];
    snprintf(m, sizeof(m), "text '%s' spans x[%d,%d] y[%d,%d] -- outside the 240x240 panel",
             o.text.c_str(), r.x, r.x + r.w, r.y, r.y + r.h);
    TEST_ASSERT_TRUE_MESSAGE(r.x >= 0 && r.x + r.w <= kSize && r.y >= 0 &&
                             r.y + r.h <= kSize, m);
  }
  // Filled rectangles too -- label backings are positioned from text metrics
  // and can run off the edge independently of the glyphs.
  for (const auto& o : g_gfx.of(DrawOp::FillRect)) {
    if (o.w > 200 || o.h > 200) continue;        // the full-span crosshairs
    char m[160];
    snprintf(m, sizeof(m), "fillRect x[%d,%d] y[%d,%d] is outside the 240x240 panel",
             o.x, o.x + o.w, o.y, o.y + o.h);
    TEST_ASSERT_TRUE_MESSAGE(o.x >= -2 && o.x + o.w <= kSize + 2 && o.y >= -2 &&
                             o.y + o.h <= kSize + 2, m);
  }
  // The cardinals are exempt from the bounds check, but must still be close to
  // the edge rather than arbitrarily off-panel.
  for (const auto& o : g_gfx.of(DrawOp::Text)) {
    if (!(o.text.size() == 1 && strchr("NSEW", o.text[0]))) continue;
    char m[128];
    const Rect r = textRect(o);
    snprintf(m, sizeof(m), "cardinal '%s' box y[%d,%d] is more than a few px off-panel",
             o.text.c_str(), r.y, r.y + r.h);
    TEST_ASSERT_TRUE_MESSAGE(r.y > -6 && r.y + r.h < kSize + 6, m);
  }
}

// --------------------------------------------- pure geometry helpers ------

// The whole point of segmentIntersectsDisc is the chord case: BOTH endpoints
// outside the disc but the segment crossing it. Driven through rebuildCache it
// only ever sees the trivial inside/far-away cases.
static void test_segment_disc_intersection_handles_the_chord_case() {
  using ui::runway::segmentIntersectsDisc;
  const int cx = kCenterX, cy = kCenterY, r = kGridOuterRadius;
  TEST_ASSERT_TRUE_MESSAGE(segmentIntersectsDisc(cx - 400, cy, cx + 400, cy),
      "a line straight through the centre must intersect");
  TEST_ASSERT_TRUE_MESSAGE(segmentIntersectsDisc(cx, cy, cx + 400, cy),
      "an endpoint inside must intersect");
  TEST_ASSERT_FALSE_MESSAGE(segmentIntersectsDisc(cx - 400, cy + r + 40, cx + 400, cy + r + 40),
      "a line passing well clear must not");
  TEST_ASSERT_FALSE_MESSAGE(segmentIntersectsDisc(cx + 400, cy, cx + 500, cy),
      "a segment entirely beyond the disc must not, even though its infinite "
      "line would cross");
  TEST_ASSERT_FALSE_MESSAGE(segmentIntersectsDisc(cx + 400, cy, cx + 400, cy),
      "a degenerate zero-length segment outside must not");
}

static void test_speed_vector_length_boundaries() {
  using ui::speedLineLengthPx;
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, speedLineLengthPx(0.0f),
      "a stationary target has no vector");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, speedLineLengthPx(-5.0f),
      "a negative ground speed must not produce a vector");
  TEST_ASSERT_TRUE_MESSAGE(speedLineLengthPx(3.0f) >= kAircraftSpeedLineMinPx,
      "a barely-moving target still gets the minimum visible stub");
  TEST_ASSERT_TRUE_MESSAGE(speedLineLengthPx(500.0f) > speedLineLengthPx(200.0f),
      "faster targets must draw longer vectors");
}

// The centre dot and the palette could both be deleted with the suite green.

static void test_the_centre_dot_is_drawn() {
  Target t[] = {{2.0f, 0.0f, 200, 90, 0.1f, "AAA111"}};
  publishTargets(t, 1);
  radarDisplayDraw();
  bool found = false;
  for (const auto& o : g_gfx.of(DrawOp::SmoothCircle))
    if (o.r == kCenterDotRadius && o.x == kCenterX && o.y == kCenterY) found = true;
  TEST_ASSERT_TRUE_MESSAGE(found, "the white dot marking your own position must be drawn");
}

// GC9A01 modules are wired BGR, so initPalette() swaps R and B for the aircraft
// colour: "logical red renders red on screen". Comparing drawn ops against the
// same mutable global initPalette writes would prove nothing, so this pins the
// literal encoding.
static void test_the_aircraft_colour_is_bgr_swapped_for_this_panel() {
  Target t[] = {{2.0f, 0.0f, 200, 90, 0.1f, "AAA111"}};
  publishTargets(t, 1);
  radarDisplayDraw();
  const uint16_t expected = config::kDisplayRgbOrder
      ? lgfx::LGFXBase::color565(kAircraftB, kAircraftG, kAircraftR)
      : lgfx::LGFXBase::color565(kAircraftR, kAircraftG, kAircraftB);
  TEST_ASSERT_EQUAL_HEX16_MESSAGE(expected, kColorAircraft,
      "dropping the R/B swap makes every aircraft render blue on the panel");
  TEST_ASSERT_EQUAL_HEX16_MESSAGE(expected, g_gfx.of(DrawOp::Triangle)[0].color,
      "and the symbol must actually be drawn in it");
}

// Rim dots are painted far-first so nearer contacts sit on top.
static void test_rim_dots_are_painted_far_first() {
  saveRunwaysFromPortal("");
  Target t[] = {{20.0f, 20.0f, 300, 45, 0.1f, "NEARER"},
                {80.0f, 80.0f, 300, 45, 0.1f, "FARTHER"}};
  publishTargets(t, 2);
  radarDisplayDraw();
  std::vector<DrawOp> dots;
  for (const auto& o : g_gfx.of(DrawOp::SmoothCircle))
    if (o.r == kBeyondRingDotRadiusPx) dots.push_back(o);
  TEST_ASSERT_EQUAL_INT_MESSAGE(2, (int)dots.size(), "both distant targets get a rim dot");
  const int d0 = radar::distSqFromCenter(dots[0].x, dots[0].y);
  const int d1 = radar::distSqFromCenter(dots[1].x, dots[1].y);
  TEST_ASSERT_TRUE_MESSAGE(d0 >= d1,
      "the farther contact must be painted first so the nearer one wins overlaps");
}

int main(int, char**) {
  UNITY_BEGIN();
  // These two must run first: the frame sprite is created once and cached in a
  // file-static, so once any test succeeds in allocating it, a scripted
  // allocation failure can never be observed again.
  RUN_TEST(test_sprite_failure_falls_back_to_direct_drawing);
  RUN_TEST(test_sprite_allocation_is_not_retried_every_frame);
  RUN_TEST(test_tags_never_overlap_each_other);
  RUN_TEST(test_a_tag_stays_next_to_its_own_symbol);
  RUN_TEST(test_the_nearest_aircraft_keeps_its_label);
  RUN_TEST(test_target_advances_along_its_track_between_fetches);
  RUN_TEST(test_extrapolation_is_clamped_on_the_total_age);
  RUN_TEST(test_a_stale_fix_is_dimmed_not_hidden);
  RUN_TEST(test_a_stalled_feed_dims_then_clears);
  RUN_TEST(test_a_normal_frame_is_blitted_once);
  RUN_TEST(test_crosshairs_are_rectangles_not_antialiased_lines);
  RUN_TEST(test_grid_has_the_expected_rings);
  RUN_TEST(test_speed_vectors_are_drawn_for_moving_traffic);
  RUN_TEST(test_a_stationary_target_has_no_speed_vector);
  RUN_TEST(test_beyond_ring_traffic_becomes_a_rim_dot);
  RUN_TEST(test_tag_lines_carry_their_distinct_colours);
  RUN_TEST(test_crosshairs_are_centred_on_the_radar_origin);
  RUN_TEST(test_cardinal_labels_are_in_the_right_places);
  RUN_TEST(test_the_scale_label_follows_the_range_and_units);
  RUN_TEST(test_a_locked_aircraft_list_suppresses_the_blit);
  RUN_TEST(test_the_grid_is_cleared_every_frame);
  RUN_TEST(test_smooth_font_is_actually_selected);
  RUN_TEST(test_the_vlw_size_search_converges_on_the_target_height);
  RUN_TEST(test_the_scale_label_is_smaller_than_the_cardinals);
  RUN_TEST(test_tags_never_overlap_with_the_smooth_font_either);
  RUN_TEST(test_runway_labels_render_with_the_smooth_font);
  RUN_TEST(test_no_text_or_rect_is_drawn_outside_the_panel);
  RUN_TEST(test_segment_disc_intersection_handles_the_chord_case);
  RUN_TEST(test_speed_vector_length_boundaries);
  RUN_TEST(test_the_centre_dot_is_drawn);
  RUN_TEST(test_the_aircraft_colour_is_bgr_swapped_for_this_panel);
  RUN_TEST(test_rim_dots_are_painted_far_first);
  RUN_TEST(test_runways_are_drawn_at_a_real_airport);
  RUN_TEST(test_no_runways_when_the_overlay_is_switched_off);
  RUN_TEST(test_no_runways_in_the_middle_of_the_ocean);
  RUN_TEST(test_cached_geometry_is_stable_across_frames);
  RUN_TEST(test_cache_rebuilds_when_the_range_changes);
  RUN_TEST(test_cache_rebuilds_when_the_location_changes);
  RUN_TEST(test_the_shipped_dataset_never_reaches_the_caps);
  RUN_TEST(test_an_in_range_airport_is_drawn_and_identified);
  return UNITY_END();
}
