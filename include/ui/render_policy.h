#pragma once

namespace ui {

/**
 * Decides when loop() composites a frame, and records what actually reached the
 * panel. Split out of main.cpp so the sequence can be tested without a display:
 * two separate bugs lived here — the last aircraft staying burned on the panel
 * after the sky emptied, and a frame that was composited but never blitted
 * being recorded as painted, which re-opened the first bug.
 *
 * Invariants:
 *  - while traffic exists, frames are requested
 *  - exactly one frame is requested after traffic disappears (to clear it)
 *  - once that clearing frame is on the panel, no more are requested
 *  - a frame that was NOT blitted never updates the record, and is retried
 */
class RenderPolicy {
 public:
  /** Should a frame be composited now? (Caller applies its own rate limit.) */
  bool shouldRender(bool traffic) const {
    return traffic || traffic_drawn_ || needs_redraw_;
  }

  /**
   * Report the outcome. `traffic` must be the value sampled BEFORE drawing:
   * a publish landing during the blit would otherwise record "no traffic" for
   * a frame that is showing some, stranding those symbols.
   */
  void onFrameDrawn(bool traffic, bool blitted) {
    if (!blitted) {
      needs_redraw_ = true;
      return;
    }
    traffic_drawn_ = traffic;
    needs_redraw_ = false;
  }

  /** Connection lost: nothing on the panel can be trusted as current. */
  void reset() {
    traffic_drawn_ = false;
    needs_redraw_ = false;
  }

  bool trafficDrawn() const { return traffic_drawn_; }
  bool needsRedraw() const { return needs_redraw_; }

 private:
  bool traffic_drawn_ = false;
  bool needs_redraw_ = false;
};

}  // namespace ui
