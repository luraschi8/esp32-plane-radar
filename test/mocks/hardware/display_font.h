// The real display_font.cpp links against the VLW blob embedded by the build,
// which does not exist on the host. Tests use the bitmap-font path (the same
// fallback the firmware takes when the smooth font fails to load).
#pragma once
#include <LovyanGFX.hpp>
extern bool g_font_is_smooth;
inline bool displayFontInit() { return g_font_is_smooth; }
inline bool displayFontIsSmooth() { return g_font_is_smooth; }
inline bool displayFontEnsureLoaded(lgfx::LGFXBase&) { return g_font_is_smooth; }
inline void displayFontSetSmoothSize(lgfx::LGFXBase& g, float s) {
  // The VLW path scales a single face; clearing the bitmap font makes
  // fontHeight() fall back to the size-scaled base, which is what the real
  // smooth font does and what findVlwSizeForHeight() binary-searches over.
  g.setFont(nullptr);
  g.setTextSize(s);
}
inline void displayFontSetBitmap(lgfx::LGFXBase& g, const lgfx::GFXfont* f) {
  g.setFont(f); g.setTextSize(1);
}
