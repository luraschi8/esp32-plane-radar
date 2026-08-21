#pragma once

namespace ui {

/**
 * Draw the full radar frame. Returns false if it was composited but not blitted
 * (the aircraft list was locked) -- callers must not record the panel as
 * updated in that case, or a skipped frame is silently lost.
 */
bool radarDisplayDraw();

/** Re-composite with fresh aircraft data. Same false-means-not-blitted contract. */
bool radarDisplayRefreshAircraft();

}  // namespace ui
