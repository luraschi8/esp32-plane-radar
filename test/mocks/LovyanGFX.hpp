// Recording-canvas stand-in for LovyanGFX. Every draw call is appended to a log
// the tests can assert over: what was drawn, where, in what colour and in what
// order. Geometry and layout are then verifiable on the host without a panel.
//
// It cannot tell you the result LOOKS right -- only hardware and a human can.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>
#include <cmath>

enum textdatum_t {
  top_left = 0, top_center, top_right,
  middle_left, middle_center, middle_right,
  bottom_left, bottom_center, bottom_right,
};

/** One recorded primitive. */
struct DrawOp {
  enum Kind { FillScreen, FillRect, Circle, SmoothCircle, WideLine, Triangle, Text, Push };
  Kind kind;
  int x = 0, y = 0, w = 0, h = 0, x2 = 0, y2 = 0, x3 = 0, y3 = 0;
  int r = 0;
  float half_width = 0.0f;
  uint16_t color = 0;
  textdatum_t datum = top_left;
  std::string text;
};

/** Tunables + the shared draw log. Reset between tests. */
struct GfxRecorder {
  std::vector<DrawOp> ops;
  bool sprite_alloc_fails = false;
  int sprite_alloc_attempts = 0;
  int char_width = 7;          // deterministic text metrics
  int line_height = 16;
  /** Clears the log only. Scripted behaviour survives, so a mid-test reset()
   *  cannot silently re-enable an allocation the test deliberately disabled. */
  void reset() { ops.clear(); }
  void resetAll() { ops.clear(); sprite_alloc_fails = false; sprite_alloc_attempts = 0; }
  size_t count(DrawOp::Kind k) const {
    size_t n = 0; for (const auto& o : ops) if (o.kind == k) ++n; return n;
  }
  std::vector<DrawOp> of(DrawOp::Kind k) const {
    std::vector<DrawOp> v; for (const auto& o : ops) if (o.kind == k) v.push_back(o); return v;
  }
};
extern GfxRecorder g_gfx;

namespace lgfx {

struct GFXfont { const char* name; int height; };

class IFont {
 public:
  enum class font_type_t { ft_unknown, ft_glcd, ft_bmp, ft_gfx, ft_vlw };
  virtual ~IFont() = default;
  virtual font_type_t getType() const { return font_type_t::ft_gfx; }
};

class LGFXBase {
 public:
  virtual ~LGFXBase() = default;

  void fillScreen(uint16_t c) { g_gfx.ops.push_back({DrawOp::FillScreen, 0,0,0,0,0,0,0,0,0,0.0f,c}); }
  void fillRect(int x, int y, int w, int h, uint16_t c) {
    DrawOp o{DrawOp::FillRect}; o.x=x; o.y=y; o.w=w; o.h=h; o.color=c; g_gfx.ops.push_back(o);
  }
  void drawCircle(int x, int y, int r, uint16_t c) {
    DrawOp o{DrawOp::Circle}; o.x=x; o.y=y; o.r=r; o.color=c; g_gfx.ops.push_back(o);
  }
  void fillCircle(int x, int y, int r, uint16_t c) {
    DrawOp o{DrawOp::SmoothCircle}; o.x=x; o.y=y; o.r=r; o.color=c; g_gfx.ops.push_back(o);
  }
  void fillSmoothCircle(int x, int y, int r, uint16_t c) {
    DrawOp o{DrawOp::SmoothCircle}; o.x=x; o.y=y; o.r=r; o.color=c; g_gfx.ops.push_back(o);
  }
  void drawWideLine(int x, int y, int x2, int y2, float hw, uint16_t c) {
    DrawOp o{DrawOp::WideLine}; o.x=x; o.y=y; o.x2=x2; o.y2=y2; o.half_width=hw; o.color=c;
    g_gfx.ops.push_back(o);
  }
  void fillTriangle(int x, int y, int x2, int y2, int x3, int y3, uint16_t c) {
    DrawOp o{DrawOp::Triangle}; o.x=x; o.y=y; o.x2=x2; o.y2=y2; o.x3=x3; o.y3=y3; o.color=c;
    g_gfx.ops.push_back(o);
  }
  void drawString(const char* s, int x, int y) {
    DrawOp o{DrawOp::Text}; o.x=x; o.y=y; o.color=text_color_; o.datum=datum_; o.text=s ? s : "";
    o.w = textWidth(s); o.h = fontHeight();
    g_gfx.ops.push_back(o);
  }

  void setTextDatum(textdatum_t d) { datum_ = d; }
  void setTextColor(uint16_t fg, uint16_t /*bg*/) { text_color_ = fg; }
  void setTextSize(float s) { text_size_ = s > 0.0f ? s : 1.0f; }
  void setFont(const GFXfont* f) { font_ = f; }
  const IFont* getFont() const { return nullptr; }
  bool loadFont(const uint8_t*, IFont::font_type_t) { return true; }

  /**
   * Proportional, so a layout that only works for monospace text fails here as
   * it would on the panel. Crude, but not a lie about glyph widths.
   */
  static int glyphWidth(char c) {
    if (c == 'M' || c == 'W' || c == 'm' || c == 'w') return 11;
    if (c == 'I' || c == 'i' || c == 'l' || c == '1' || c == '.' || c == ' ') return 4;
    return 7;
  }
  int textWidth(const char* s) const {
    if (!s) return 0;
    int w = 0;
    for (const char* p = s; *p; ++p) w += glyphWidth(*p);
    return (int)(w * text_size_);
  }
  /**
   * Honours the SELECTED font. Returning a constant made pickGfxFontClosest()
   * measure every candidate identically, so it always chose candidates[0] --
   * the font-selection logic ran but was never actually tested.
   */
  int fontHeight() const {
    const int base = font_ ? font_->height : g_gfx.line_height;
    return (int)(base * text_size_);
  }

  static uint16_t color565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
  }
  void setTextWrap(bool) {}
  void setRotation(int) {}
  void setBrightness(uint8_t) {}
  bool init() { return true; }

 protected:
  textdatum_t datum_ = top_left;
  uint16_t text_color_ = 0xFFFF;
  float text_size_ = 1.0f;
  const GFXfont* font_ = nullptr;
};

using LovyanGFX = LGFXBase;

// --- the pieces include/hardware/lgfx_config.hpp constructs ---
struct SpiCfg { int spi_host=0; uint32_t freq_write=0; int pin_sclk=-1, pin_mosi=-1, pin_miso=-1, pin_dc=-1; };
struct PanelCfg { int pin_cs=-1, pin_rst=-1; bool invert=false, rgb_order=false; };
class Bus_SPI { public: SpiCfg config() const { return cfg_; } void config(const SpiCfg& c) { cfg_ = c; } SpiCfg cfg_; };
class Panel_GC9A01 {
 public:
  PanelCfg config() const { return cfg_; }
  void config(const PanelCfg& c) { cfg_ = c; }
  void setBus(Bus_SPI*) {}
  PanelCfg cfg_;
};
class LGFX_Device : public LGFXBase { public: void setPanel(Panel_GC9A01*) {} };

}  // namespace lgfx

#define SPI2_HOST 1

class LGFX_Sprite : public lgfx::LGFXBase {
 public:
  explicit LGFX_Sprite(lgfx::LGFXBase* = nullptr) {}
  void setColorDepth(int d) { depth_ = d; }
  bool createSprite(int w, int h) {
    mockEvent("sprite_alloc");
    ++g_gfx.sprite_alloc_attempts;
    if (g_gfx.sprite_alloc_fails) return false;
    w_ = w; h_ = h; return true;
  }
  void pushSprite(int x, int y) {
    DrawOp o{DrawOp::Push}; o.x=x; o.y=y; o.w=w_; o.h=h_; g_gfx.ops.push_back(o);
  }
  int depth_ = 16, w_ = 0, h_ = 0;
};
