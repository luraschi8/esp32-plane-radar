#include "ui/runway_overlay.h"

#include <lgfx/v1/lgfx_fonts.hpp>

#include <cmath>
#include <cstdlib>

#include "data/large_airports.h"
#include "hardware/display_font.h"
#include "services/radar_location.h"
#include "ui/radar_geo.h"
#include "ui/radar_range.h"
#include "ui/radar_theme.h"

namespace ui::runway {
namespace {

/** Exhaustive search over the dataset (every airport plus every pairwise circle
 * intersection as a candidate centre) gives a worst case of 4 airports (Moscow)
 * and 12 strips (KORD+KMDW) inside the widest preset's 36.8 km disc. */
constexpr size_t kMaxAirportLabels = 12;

bool s_runway_label_ready = false;
bool s_runway_label_use_vlw = false;
float s_runway_label_vlw_size = 0.38f;
const lgfx::GFXfont* s_runway_label_gfx = &fonts::FreeSansBold12pt7b;

int measureVlwHeight(lgfx::LGFXBase& gfx, float size) {
  gfx.setTextSize(size);
  return gfx.fontHeight();
}

float findVlwSizeForHeight(lgfx::LGFXBase& gfx, int target_px) {
  float lo = 0.2f;
  float hi = 1.2f;
  for (int i = 0; i < 14; ++i) {
    const float mid = (lo + hi) * 0.5f;
    if (measureVlwHeight(gfx, mid) < target_px) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return hi;
}

void initRunwayLabelStyle(lgfx::LGFXBase& gfx) {
  if (s_runway_label_ready) {
    return;
  }

  const int target = radar::kRunwayLabelHeightPx;
  if (displayFontIsSmooth()) {
    s_runway_label_use_vlw = true;
    s_runway_label_vlw_size = findVlwSizeForHeight(gfx, target);
  } else {
    s_runway_label_gfx = &fonts::FreeSansBold12pt7b;
    s_runway_label_use_vlw = false;
  }
  s_runway_label_ready = true;
}

void applyRunwayLabelStyle(lgfx::LGFXBase& gfx) {
  if (s_runway_label_use_vlw) {
    displayFontSetSmoothSize(gfx, s_runway_label_vlw_size);
  } else {
    displayFontSetBitmap(gfx, s_runway_label_gfx);
  }
}

float e7ToDeg(int32_t e7) { return static_cast<float>(e7) * 1e-7f; }

bool segmentIntersectsDisc(int x0, int y0, int x1, int y1) {
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int r = radar::kGridOuterRadius;
  const int r_sq = r * r;

  if (radar::distSqFromCenter(x0, y0) <= r_sq ||
      radar::distSqFromCenter(x1, y1) <= r_sq) {
    return true;
  }

  const int dx = x1 - x0;
  const int dy = y1 - y0;
  const int fx = x0 - cx;
  const int fy = y0 - cy;
  // 64-bit for the discriminant: b*b overflows int32 once a segment endpoint is
  // more than a few hundred pixels out. Today rebuildCache() pre-filters
  // airports to the fetch radius so that cannot happen, but that safety lives in
  // a different function -- a narrower preset or a wider disc would reach it.
  // This runs on cache rebuild only, never per frame.
  const int64_t a = static_cast<int64_t>(dx) * dx + static_cast<int64_t>(dy) * dy;
  if (a == 0) {
    return false;
  }
  const int64_t b = 2 * (static_cast<int64_t>(fx) * dx + static_cast<int64_t>(fy) * dy);
  const int64_t c =
      static_cast<int64_t>(fx) * fx + static_cast<int64_t>(fy) * fy - r_sq;
  const int64_t disc_sq = b * b - 4 * a * c;
  if (disc_sq < 0) {
    return false;
  }
  const float disc = sqrtf(static_cast<float>(disc_sq));
  const float inv2a = 1.0f / (2.0f * static_cast<float>(a));
  const float t0 = (-static_cast<float>(b) - disc) * inv2a;
  const float t1 = (-static_cast<float>(b) + disc) * inv2a;
  return (t0 >= 0.0f && t0 <= 1.0f) || (t1 >= 0.0f && t1 <= 1.0f);
}

void drawBoldRunwayLabel(lgfx::LGFXBase& gfx, const char* ident, int mx, int my) {
  const int tw = gfx.textWidth(ident);
  const int th = gfx.fontHeight();
  constexpr int kPadX = 2;
  constexpr int kPadY = 1;

  gfx.setTextDatum(textdatum_t::bottom_center);
  // The anchor is pushed outward from the centre and, for an airport beyond the
  // ring, clipped onto the ring itself -- which at the top of the panel leaves
  // only a few pixels above it. The label is drawn upward from the anchor, so
  // without this it runs off the edge and is cut in half. Clamp here, where the
  // metrics are already in hand; one branch per label, no extra work.
  const int half_w = tw / 2 + kPadX;
  if (mx - half_w < 0) mx = half_w;
  if (mx + half_w > radar::kSize) mx = radar::kSize - half_w;
  if (my - th - kPadY < 0) my = th + kPadY;
  if (my > radar::kSize) my = radar::kSize;

  const int left = mx - tw / 2 - kPadX;
  const int top = my - th - kPadY;
  gfx.fillRect(left, top, tw + kPadX * 2, th + kPadY, radar::kColorBackground);
  gfx.setTextColor(radar::kColorRunwayLabel, radar::kColorBackground);
  gfx.drawString(ident, mx - 1, my);
  gfx.drawString(ident, mx + 1, my);
  gfx.drawString(ident, mx, my);
}

/** Screen-space runway geometry; rebuilt only when the view changes. */
struct CachedSegment {
  int16_t x0, y0, x1, y1;
};
struct CachedLabel {
  int16_t x, y;
  uint16_t airport_idx;
};

// Overridable so tests can drive the truncation path: with the shipped dataset
// the worst case anywhere on Earth is 12 strips, so 32 is never reached and the
// label-before-cap ordering would otherwise be untestable.
#ifndef RUNWAY_MAX_CACHED_SEGMENTS
#define RUNWAY_MAX_CACHED_SEGMENTS 32
#endif
constexpr size_t kMaxCachedSegments = RUNWAY_MAX_CACHED_SEGMENTS;

CachedSegment s_segments[kMaxCachedSegments];
size_t s_segment_count = 0;
CachedLabel s_labels[kMaxAirportLabels];
size_t s_label_count = 0;

bool s_cache_valid = false;
float s_cache_outer_km = 0.0f;
double s_cache_lat = 0.0;
double s_cache_lon = 0.0;

/** Project and clip one runway; false when it misses the radar disc. */
bool computeRunwayLine(const data::large_airports::Runway& rw, int* x0, int* y0,
                       int* x1, int* y1) {
  radar::latLonToScreen(e7ToDeg(rw.le_lat_e7), e7ToDeg(rw.le_lon_e7), x0, y0);
  radar::latLonToScreen(e7ToDeg(rw.he_lat_e7), e7ToDeg(rw.he_lon_e7), x1, y1);

  if (!segmentIntersectsDisc(*x0, *y0, *x1, *y1)) {
    return false;
  }

  radar::clipPointToOuterRing(*x0, *y0, x1, y1);
  radar::clipPointToOuterRing(*x1, *y1, x0, y0);
  return true;
}

void offsetLabelFromCenter(int ax, int ay, int* lx, int* ly) {
  const int dx = ax - radar::kCenterX;
  const int dy = ay - radar::kCenterY;
  const float len = sqrtf(static_cast<float>(dx * dx + dy * dy));
  const int gap = radar::kRunwayLabelGapPx;
  if (len < 1.0f) {
    *lx = ax;
    *ly = ay - gap;
    return;
  }
  *lx = ax + static_cast<int>(lroundf(dx / len * static_cast<float>(gap)));
  *ly = ay + static_cast<int>(lroundf(dy / len * static_cast<float>(gap)));
}

void clipPointOntoOuterRing(int* x, int* y) {
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int r = radar::kGridOuterRadius;
  const int dx = *x - cx;
  const int dy = *y - cy;
  const int d_sq = dx * dx + dy * dy;
  const int r_sq = r * r;
  if (d_sq <= r_sq || d_sq == 0) {
    return;
  }
  const float scale = static_cast<float>(r) / sqrtf(static_cast<float>(d_sq));
  *x = cx + static_cast<int>(lroundf(static_cast<float>(dx) * scale));
  *y = cy + static_cast<int>(lroundf(static_cast<float>(dy) * scale));
}

void computeAirportLabelPos(const data::large_airports::Airport& ap, int* lx,
                            int* ly) {
  int ax = 0;
  int ay = 0;
  radar::latLonToScreen(e7ToDeg(ap.lat_e7), e7ToDeg(ap.lon_e7), &ax, &ay);
  clipPointOntoOuterRing(&ax, &ay);
  offsetLabelFromCenter(ax, ay, lx, ly);
}

bool cacheMatchesView() {
  return s_cache_valid && s_cache_outer_km == radar::rangeCurrent().outer_km &&
         s_cache_lat == services::location::lat() &&
         s_cache_lon == services::location::lon();
}

bool airportAlreadyLabelled(uint16_t ap_idx) {
  for (size_t i = 0; i < s_label_count; ++i) {
    if (s_labels[i].airport_idx == ap_idx) {
      return true;
    }
  }
  return false;
}

/**
 * Walk the whole dataset once and keep only what is on screen. This is the
 * expensive part (1706 runways x projection, measured at ~34 ms) and its result
 * only changes when the range preset or the radar centre moves, so it must not
 * run per frame.
 */
void rebuildCache() {
  s_segment_count = 0;
  s_label_count = 0;

  const float radius_km = radar::fetchRadiusKm();
  uint16_t checked_ap = 0xFFFF;
  bool ap_in_range = false;
  bool truncated = false;

  for (size_t i = 0; i < data::large_airports::kRunwayCount; ++i) {
    const auto& rw = data::large_airports::kRunways[i];
    const uint16_t ap_idx = rw.airport_idx;

    // Runways are generated grouped by airport, so this evaluates the distance
    // once per airport rather than once per strip.
    if (ap_idx != checked_ap) {
      checked_ap = ap_idx;
      const auto& ap = data::large_airports::kAirports[ap_idx];
      float dx_km = 0.0f;
      float dy_km = 0.0f;
      float dist_km = 0.0f;
      radar::offsetKmFromCenter(e7ToDeg(ap.lat_e7), e7ToDeg(ap.lon_e7), &dx_km,
                                &dy_km, &dist_km);
      ap_in_range = dist_km <= radius_km;
    }
    if (!ap_in_range) {
      continue;
    }

    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    if (!computeRunwayLine(rw, &x0, &y0, &x1, &y1)) {
      continue;
    }
    // Label first, then the strip. kRunways is ordered by ICAO rather than by
    // distance, so if the strip cache fills, the airports that miss out are
    // whichever sort late -- possibly the closest one. Collecting the label
    // before the cap check keeps every in-range airport identified even when
    // its strips are dropped. (Doing this after the cap check made the cap
    // behave exactly like an early break.)
    if (s_label_count < kMaxAirportLabels && !airportAlreadyLabelled(ap_idx)) {
      int lx = 0;
      int ly = 0;
      computeAirportLabelPos(data::large_airports::kAirports[ap_idx], &lx, &ly);
      s_labels[s_label_count++] = {static_cast<int16_t>(lx),
                                   static_cast<int16_t>(ly), ap_idx};
    }

    if (s_segment_count >= kMaxCachedSegments) {
      truncated = true;
      continue;
    }
    s_segments[s_segment_count++] = {
        static_cast<int16_t>(x0), static_cast<int16_t>(y0),
        static_cast<int16_t>(x1), static_cast<int16_t>(y1)};
  }

  if (truncated) {
    Serial.printf("runway: strip cache full at %u; labels still collected\n",
                  static_cast<unsigned>(kMaxCachedSegments));
  }

  s_cache_outer_km = radar::rangeCurrent().outer_km;
  s_cache_lat = services::location::lat();
  s_cache_lon = services::location::lon();
  s_cache_valid = true;
}

}  // namespace

void drawLargeAirportRunways(lgfx::LGFXBase& gfx) {
  if (!radar::showRunways()) {
    s_cache_valid = false;  // re-scan when the overlay is switched back on
    return;
  }
  displayFontEnsureLoaded(gfx);

  if (!cacheMatchesView()) {
    rebuildCache();
  }

  for (size_t i = 0; i < s_segment_count; ++i) {
    const CachedSegment& sg = s_segments[i];
    gfx.drawWideLine(sg.x0, sg.y0, sg.x1, sg.y1, radar::kRunwayLineHalfWidth,
                     radar::kColorRunway);
  }

  if (s_label_count == 0) {
    return;
  }
  initRunwayLabelStyle(gfx);
  applyRunwayLabelStyle(gfx);
  for (size_t i = 0; i < s_label_count; ++i) {
    drawBoldRunwayLabel(gfx,
                        data::large_airports::kAirports[s_labels[i].airport_idx]
                            .ident,
                        s_labels[i].x, s_labels[i].y);
  }
}

}  // namespace ui::runway
