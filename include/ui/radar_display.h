#pragma once

namespace ui {

/** Draw the static sonar/radar grid (black disc, green overlay, labels). */
void radarDisplayDraw();

/** Re-composite the frame with fresh aircraft data (grid included; no flicker). */
void radarDisplayRefreshAircraft();

}  // namespace ui
