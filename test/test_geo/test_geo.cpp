// Exercises the SHIPPED radar_geo.cpp (included directly so file-local state is
// reachable), not a reimplementation of it.
#include <Arduino.h>   // mocks first: the shipped sources rely on it transitively
#include <unity.h>
#include <cmath>

#include "fixtures_geo.h"
#include "../mocks/mock_globals.h"
#include "../../src/services/radar_location.cpp"
#include "../../src/ui/radar_range.cpp"
#include "../../src/ui/radar_geo.cpp"

using namespace ui::radar;

static void setCenter(double lat, double lon) {
  char a[32], b[32];
  snprintf(a, sizeof(a), "%.6f", lat);
  snprintf(b, sizeof(b), "%.6f", lon);
  TEST_ASSERT_TRUE(services::location::saveFromStrings(a, b));
}

void setUp() { g_nvs.reset(); mockSetMs(1000); services::location::clear(); rangeInit(); }
void tearDown() {}

// --- the bug that started all this: longitude must scale by cos(latitude) ---
static void test_longitude_scaled_by_cos_latitude() {
  setCenter(40.445564, -3.698361);
  float dx = 0, dy = 0, dist = 0;
  offsetKmFromCenter(40.445564f, -3.698361f + 1.0f, &dx, &dy, &dist);
  const float expected = 111.0f * cosf(40.445564f * 0.01745329252f);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, expected, dx);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, dy);
  // Without the correction this would be a flat 111 km — the original defect.
  TEST_ASSERT_TRUE(dx < 90.0f);
}

// --- checked against the API's own dst/dir for 14 real aircraft ---
static void test_matches_api_ground_truth() {
  setCenter(kFixtureCenterLat, kFixtureCenterLon);
  for (int i = 0; i < kGeoFixtureCount; ++i) {
    const GeoFixture& f = kGeoFixtures[i];
    float dx = 0, dy = 0, dist = 0;
    offsetKmFromCenter(f.lat, f.lon, &dx, &dy, &dist);
    const float dist_nm = dist / 1.852f;
    const float bearing = fmodf(atan2f(dx, dy) * 57.2957795f + 360.0f, 360.0f);
    char msg[96];
    snprintf(msg, sizeof(msg), "fixture %d distance: api=%.3f ours=%.3f", i, f.dst_nm, dist_nm);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f, f.dst_nm, dist_nm, msg);
    snprintf(msg, sizeof(msg), "fixture %d bearing: api=%.2f ours=%.2f", i, f.dir_deg, bearing);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.6f, f.dir_deg, bearing, msg);
  }
}

// --- a flat 111 km/deg would fail the same fixtures: proves the test has teeth ---
static void test_ground_truth_would_reject_uncorrected_projection() {
  setCenter(kFixtureCenterLat, kFixtureCenterLon);
  int would_fail = 0;
  for (int i = 0; i < kGeoFixtureCount; ++i) {
    const GeoFixture& f = kGeoFixtures[i];
    const float dx_bad = (f.lon - (float)kFixtureCenterLon) * 111.0f;  // no cos()
    const float dy = (f.lat - (float)kFixtureCenterLat) * 111.0f;
    const float bad_nm = sqrtf(dx_bad * dx_bad + dy * dy) / 1.852f;
    if (fabsf(bad_nm - f.dst_nm) > 0.05f) ++would_fail;
  }
  TEST_ASSERT_EQUAL_MESSAGE(kGeoFixtureCount, would_fail,
                            "uncorrected projection must fail every fixture");
}

static void test_antimeridian_east_is_not_west() {
  setCenter(0.0, 179.95);
  float dx = 0, dy = 0, dist = 0;
  offsetKmFromCenter(0.0f, -179.95f, &dx, &dy, &dist);
  TEST_ASSERT_TRUE_MESSAGE(dx > 0.0f, "target east of the antimeridian must read east");
  TEST_ASSERT_FLOAT_WITHIN(2.0f, 11.1f, dx);
  TEST_ASSERT_TRUE_MESSAGE(dist < 50.0f, "must not read as most of the way round the planet");
}

static void test_antimeridian_west_direction() {
  setCenter(0.0, -179.95);
  float dx = 0, dy = 0, dist = 0;
  offsetKmFromCenter(0.0f, 179.95f, &dx, &dy, &dist);
  TEST_ASSERT_TRUE_MESSAGE(dx < 0.0f, "target west of the antimeridian must read west");
  TEST_ASSERT_FLOAT_WITHIN(2.0f, 11.1f, fabsf(dx));
}

static void test_cos_cache_invalidates_when_centre_moves() {
  setCenter(0.0, 0.0);                       // equator: scale 1.0
  float dx_eq = 0, dy = 0, dist = 0;
  offsetKmFromCenter(0.0f, 1.0f, &dx_eq, &dy, &dist);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 111.0f, dx_eq);

  setCenter(60.0, 0.0);                      // cos(60) = 0.5
  float dx_60 = 0;
  offsetKmFromCenter(60.0f, 1.0f, &dx_60, &dy, &dist);
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1.0f, 55.5f, dx_60,
                                   "stale cos cache would still report ~111 km");
}

static void test_null_dist_is_accepted() {
  setCenter(40.0, -3.0);
  float dx = -1, dy = -1;
  offsetKmFromCenter(41.0f, -3.0f, &dx, &dy, nullptr);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 111.0f, dy);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, dx);
}

static void test_centre_maps_to_screen_centre_and_north_is_up() {
  setCenter(40.0, -3.0);
  int x = 0, y = 0;
  kmOffsetToScreen(0.0f, 0.0f, &x, &y);
  TEST_ASSERT_EQUAL_INT(kCenterX, x);
  TEST_ASSERT_EQUAL_INT(kCenterY, y);
  kmOffsetToScreen(0.0f, 5.0f, &x, &y);       // 5 km north
  TEST_ASSERT_TRUE_MESSAGE(y < kCenterY, "north must be up (smaller y)");
  kmOffsetToScreen(5.0f, 0.0f, &x, &y);       // 5 km east
  TEST_ASSERT_TRUE_MESSAGE(x > kCenterX, "east must be right (larger x)");
}

static void test_px_per_km_tracks_the_range_preset() {
  const float at_default = pxPerKm();
  rangeNext();                                 // 10 km -> 15 km
  TEST_ASSERT_TRUE_MESSAGE(pxPerKm() < at_default,
                           "a wider range must map fewer pixels per km");
  TEST_ASSERT_FLOAT_WITHIN(0.01f, (float)kGridOuterRadius / rangeCurrent().outer_km, pxPerKm());
}

static void test_clip_pulls_point_inside_the_outer_ring() {
  int x1 = kCenterX + 400, y1 = kCenterY;      // far outside
  clipPointToOuterRing(kCenterX, kCenterY, &x1, &y1);
  TEST_ASSERT_TRUE(distSqFromCenter(x1, y1) <= kGridOuterRadius * kGridOuterRadius);
  int inx = kCenterX + 5, iny = kCenterY + 5;  // already inside: untouched
  clipPointToOuterRing(kCenterX, kCenterY, &inx, &iny);
  TEST_ASSERT_EQUAL_INT(kCenterX + 5, inx);
  TEST_ASSERT_EQUAL_INT(kCenterY + 5, iny);
}

static void test_clip_collapses_when_no_point_on_segment_qualifies() {
  int x1 = 5000, y1 = 5000;                    // both ends outside the disc
  clipPointToOuterRing(4000, 4000, &x1, &y1);
  TEST_ASSERT_EQUAL_INT(4000, x1);
  TEST_ASSERT_EQUAL_INT(4000, y1);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_longitude_scaled_by_cos_latitude);
  RUN_TEST(test_matches_api_ground_truth);
  RUN_TEST(test_ground_truth_would_reject_uncorrected_projection);
  RUN_TEST(test_antimeridian_east_is_not_west);
  RUN_TEST(test_antimeridian_west_direction);
  RUN_TEST(test_cos_cache_invalidates_when_centre_moves);
  RUN_TEST(test_null_dist_is_accepted);
  RUN_TEST(test_centre_maps_to_screen_centre_and_north_is_up);
  RUN_TEST(test_px_per_km_tracks_the_range_preset);
  RUN_TEST(test_clip_pulls_point_inside_the_outer_ring);
  RUN_TEST(test_clip_collapses_when_no_point_on_segment_qualifies);
  return UNITY_END();
}
