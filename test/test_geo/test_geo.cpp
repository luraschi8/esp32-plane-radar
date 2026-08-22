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
static void checkAgainstApi(const char* set, double clat, double clon,
                            const GeoFixture* fx, int n) {
  setCenter(clat, clon);
  for (int i = 0; i < n; ++i) {
    const GeoFixture& f = fx[i];
    float dx = 0, dy = 0, dist = 0;
    offsetKmFromCenter(f.lat, f.lon, &dx, &dy, &dist);
    const float dist_nm = dist / 1.852f;
    const float bearing = fmodf(atan2f(dx, dy) * 57.2957795f + 360.0f, 360.0f);
    char msg[128];
    snprintf(msg, sizeof(msg), "%s[%d] distance: api=%.3f ours=%.3f NM", set, i, f.dst_nm, dist_nm);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f, f.dst_nm, dist_nm, msg);
    // Bearings straddle 0/360, so compare the shortest angular difference.
    float diff = fmodf(fabsf(bearing - f.dir_deg), 360.0f);
    if (diff > 180.0f) diff = 360.0f - diff;
    snprintf(msg, sizeof(msg), "%s[%d] bearing: api=%.2f ours=%.2f deg", set, i, f.dir_deg, bearing);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.6f, 0.0f, diff, msg);
  }
}

static void test_matches_api_ground_truth() {
  checkAgainstApi("madrid", kMadridLat, kMadridLon, kMadridFixtures, kMadridFixtureCount);
  checkAgainstApi("amsterdam", kAmsterdamLat, kAmsterdamLon, kAmsterdamFixtures,
                  kAmsterdamFixtureCount);
}

// --- a flat 111 km/deg would fail the same fixtures: proves the test has teeth ---
// The fixtures must be tight enough that a WRONG projection is rejected. This
// runs the real code with a deliberately wrong centre latitude (which changes
// only the cos scale) and requires the ground-truth check to reject it.
static void test_fixtures_reject_a_wrong_longitude_scale() {
  int mismatches = 0;
  setCenter(kAmsterdamLat, kAmsterdamLon);
  for (int i = 0; i < kAmsterdamFixtureCount; ++i) {
    const GeoFixture& f = kAmsterdamFixtures[i];
    float dx = 0, dy = 0, dist = 0;
    offsetKmFromCenter(f.lat, f.lon, &dx, &dy, &dist);
    const float good = fabsf(dist / 1.852f - f.dst_nm);
    // Same maths, equator scale (cos = 1): what the original bug computed.
    const float dx_bad = (f.lon - (float)kAmsterdamLon) * 111.0f;
    const float bad = fabsf(sqrtf(dx_bad * dx_bad + dy * dy) / 1.852f - f.dst_nm);
    TEST_ASSERT_TRUE_MESSAGE(good <= 0.05f, "the real projection must match");
    if (bad > 0.05f) ++mismatches;
  }
  TEST_ASSERT_EQUAL_MESSAGE(kAmsterdamFixtureCount, mismatches,
      "every fixture must reject an uncorrected longitude scale, or the "
      "tolerance is too loose to detect the original bug");
}

static void test_antimeridian_east_is_not_west() {
  setCenter(0.0, 179.95);
  float dx = 0, dy = 0, dist = 0;
  offsetKmFromCenter(0.0f, -179.95f, &dx, &dy, &dist);
  TEST_ASSERT_TRUE_MESSAGE(dx > 0.0f, "target east of the antimeridian must read east");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f, 11.1f, dx, "0.1 deg at the equator is 11.1 km");
  TEST_ASSERT_TRUE_MESSAGE(dist < 50.0f, "must not read as most of the way round the planet");
}

static void test_antimeridian_west_direction() {
  setCenter(0.0, -179.95);
  float dx = 0, dy = 0, dist = 0;
  offsetKmFromCenter(0.0f, 179.95f, &dx, &dy, &dist);
  TEST_ASSERT_TRUE_MESSAGE(dx < 0.0f, "target west of the antimeridian must read west");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f, 11.1f, fabsf(dx), "0.1 deg at the equator is 11.1 km");
}

static void test_cos_cache_invalidates_when_centre_moves() {
  setCenter(0.0, 0.0);                       // equator: scale 1.0
  float dx_eq = 0, dy = 0, dist = 0;
  offsetKmFromCenter(0.0f, 1.0f, &dx_eq, &dy, &dist);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 111.0f, dx_eq);

  setCenter(60.0, 0.0);                      // cos(60) = 0.5
  float dx_60 = 0;
  offsetKmFromCenter(60.0f, 1.0f, &dx_60, &dy, &dist);
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.1f, 55.5f, dx_60,
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
  // Independently computed: the 10 km preset is 13.333 km outer over a 107 px
  // grid radius = 8.025 px/km. Restating the implementation would prove nothing.
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, 8.025f, pxPerKm(),
                                   "10 km preset: 107 px / 13.333 km");
  const float at_default = pxPerKm();
  rangeNext();                                 // 10 km -> 15 km
  TEST_ASSERT_TRUE_MESSAGE(pxPerKm() < at_default,
                           "a wider range must map fewer pixels per km");

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

// distSqFromCenter is the oracle several clipping tests measure against, so a
// bug in it makes those tests agree with the broken code. Pin it here against
// arithmetic that does not call it.
static void test_dist_sq_from_center_is_the_real_squared_distance() {
  struct Case { int x, y; };
  const int kCenterX = ui::radar::kCenterX, kCenterY = ui::radar::kCenterY;
  const Case cases[] = {{kCenterX, kCenterY}, {kCenterX + 30, kCenterY},
                        {kCenterX, kCenterY + 30}, {kCenterX - 17, kCenterY + 44},
                        {0, 0}, {239, 239}};
  for (const auto& c : cases) {
    const int dx = c.x - kCenterX, dy = c.y - kCenterY;
    const int expect = dx * dx + dy * dy;
    char m[160];
    snprintf(m, sizeof(m), "distSqFromCenter(%d,%d) with centre (%d,%d)",
             c.x, c.y, kCenterX, kCenterY);
    TEST_ASSERT_EQUAL_INT_MESSAGE(expect, ui::radar::distSqFromCenter(c.x, c.y), m);
  }
  // Both axes must contribute: dropping either term leaves the on-axis cases
  // correct, which is exactly how a dropped dy went unnoticed.
  TEST_ASSERT_TRUE_MESSAGE(
      ui::radar::distSqFromCenter(kCenterX + 3, kCenterY + 40) >
      ui::radar::distSqFromCenter(kCenterX + 3, kCenterY + 4),
      "the y term must contribute to the distance");
  TEST_ASSERT_TRUE_MESSAGE(
      ui::radar::distSqFromCenter(kCenterX + 40, kCenterY + 3) >
      ui::radar::distSqFromCenter(kCenterX + 4, kCenterY + 3),
      "the x term must contribute to the distance");
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_dist_sq_from_center_is_the_real_squared_distance);
  RUN_TEST(test_longitude_scaled_by_cos_latitude);
  RUN_TEST(test_matches_api_ground_truth);
  RUN_TEST(test_fixtures_reject_a_wrong_longitude_scale);
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
