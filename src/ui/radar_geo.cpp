#include "ui/radar_geo.h"

#include <cmath>

#include "services/radar_location.h"
#include "ui/radar_range.h"
#include "ui/radar_theme.h"

namespace ui::radar {

namespace {

constexpr float kKmPerDeg = 111.0f;
constexpr float kDegToRad = 0.01745329252f;

/** cos(centre latitude), recomputed only when the radar centre moves. */
double s_cached_center_lat = 1000.0;  // out of range: forces first compute
float s_cached_lon_scale = 1.0f;

float lonScale() {
  const double center_lat = services::location::lat();
  if (center_lat != s_cached_center_lat) {
    s_cached_center_lat = center_lat;
    s_cached_lon_scale = cosf(static_cast<float>(center_lat) * kDegToRad);
  }
  return s_cached_lon_scale;
}

}  // namespace

void offsetKmFromCenter(float lat, float lon, float* dx_km, float* dy_km,
                        float* dist_km) {
  // Normalise across the antimeridian, or a centre at 179.9 deg puts a target
  // 11 km to its east on the opposite rim.
  float dlon = static_cast<float>(lon - services::location::lon());
  if (dlon > 180.0f) {
    dlon -= 360.0f;
  } else if (dlon < -180.0f) {
    dlon += 360.0f;
  }
  *dx_km = dlon * kKmPerDeg * lonScale();
  *dy_km = static_cast<float>(lat - services::location::lat()) * kKmPerDeg;
  if (dist_km != nullptr) {
    *dist_km = sqrtf((*dx_km) * (*dx_km) + (*dy_km) * (*dy_km));
  }
}

float pxPerKm() {
  return static_cast<float>(kGridOuterRadius) / rangeCurrent().outer_km;
}

void kmOffsetToScreen(float dx_km, float dy_km, int* out_x, int* out_y) {
  const float px_per_km = pxPerKm();
  *out_x = kCenterX + static_cast<int>(lroundf(dx_km * px_per_km));
  *out_y = kCenterY - static_cast<int>(lroundf(dy_km * px_per_km));
}

void latLonToScreen(float lat, float lon, int* out_x, int* out_y) {
  const float px_per_km = pxPerKm();

  float dx_km = 0.0f;
  float dy_km = 0.0f;
  float dist_km = 0.0f;
  offsetKmFromCenter(lat, lon, &dx_km, &dy_km, &dist_km);

  *out_x = kCenterX + static_cast<int>(lroundf(dx_km * px_per_km));
  *out_y = kCenterY - static_cast<int>(lroundf(dy_km * px_per_km));
}

int distSqFromCenter(int x, int y) {
  const int dx = x - kCenterX;
  const int dy = y - kCenterY;
  return dx * dx + dy * dy;
}

void clipPointToOuterRing(int x0, int y0, int* x1, int* y1) {
  const int max_r = kGridOuterRadius;
  const int max_r_sq = max_r * max_r;
  if (distSqFromCenter(*x1, *y1) <= max_r_sq) {
    return;
  }

  const int dx = *x1 - x0;
  const int dy = *y1 - y0;
  float t = 1.0f;
  for (int step = 0; step < 20; ++step) {
    const int px = x0 + static_cast<int>(lroundf(dx * t));
    const int py = y0 + static_cast<int>(lroundf(dy * t));
    if (distSqFromCenter(px, py) <= max_r_sq) {
      *x1 = px;
      *y1 = py;
      return;
    }
    t -= 0.05f;
    if (t <= 0.0f) {
      *x1 = x0;
      *y1 = y0;
      return;
    }
  }
}

}  // namespace ui::radar
