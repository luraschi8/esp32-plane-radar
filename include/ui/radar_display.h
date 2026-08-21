#pragma once

namespace ui {

/**
 * Allocate the off-screen frame buffer. Call this FIRST, before the network
 * stack comes up: it needs 115 KB contiguous, and once WiFi and the reused TLS
 * session are established the largest free block is ~9 KB, so an allocation
 * first attempted later can never succeed.
 */
bool radarDisplayReserveFrame();

/**
 * Draw the full radar frame. Returns false if it was composited but not blitted
 * (the aircraft list was locked) -- callers must not record the panel as
 * updated in that case, or a skipped frame is silently lost.
 */
bool radarDisplayDraw();

/** Re-composite with fresh aircraft data. Same false-means-not-blitted contract. */
bool radarDisplayRefreshAircraft();

}  // namespace ui
