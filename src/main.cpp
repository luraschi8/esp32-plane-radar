/**
 * Plane Radar — WiFi setup, then radar UI on the round GC9A01 display.
 */

#include <Arduino.h>
#include <WiFi.h>

#include "config.h"
#include "debug_log.h"
#include "hardware/display.h"
#include "services/adsb_client.h"
#include "services/radar_location.h"
#include "services/wifi_setup.h"
#include "ui/radar_display.h"
#include "ui/render_policy.h"
#include "ui/radar_range.h"
#include "ui/status_screens.h"

namespace {

bool g_radar_visible = false;
unsigned long g_wifi_down_since = 0;
unsigned long g_last_reconnect_ms = 0;
unsigned long g_last_render_ms = 0;
ui::RenderPolicy g_render;
bool g_fetch_task_ok = false;
unsigned long g_last_task_retry_ms = 0;

void showRadarIfConnected() {
  if (WiFi.status() != WL_CONNECTED) {
    g_radar_visible = false;
    return;
  }
  // Sample before the blit: pushSprite takes ~11.5 ms, and a publish landing in
  // that window would otherwise latch "no traffic" for a frame that is showing
  // some -- stranding those symbols on an idle screen.
  const bool traffic = services::adsb::hasTraffic();
  const bool blitted = ui::radarDisplayDraw();
  g_render.onFrameDrawn(traffic, blitted);
  if (!blitted) {
    return;  // loop() retries; never latch over a status screen
  }
  g_radar_visible = true;
}

void onRangeTap() {
  ui::radar::rangeNext();
  char range_label[12];
  ui::radar::formatCurrentRing3Label(range_label, sizeof(range_label));
  Serial.printf("Range: %s (outer ~%.0f km)\n", range_label,
                ui::radar::rangeCurrent().outer_km);

  if (g_radar_visible && WiFi.status() == WL_CONNECTED) {
    // Sample before drawing; a declined draw leaves needs_redraw set so the
    // rings and scale label cannot be stranded on the previous preset.
    const bool traffic = services::adsb::hasTraffic();
    g_render.onFrameDrawn(traffic, ui::radarDisplayDraw());
  }
}

void handleBootButton() {
  bootButtonPollLongPress();
  if (bootButtonConsumeTap()) {
    onRangeTap();
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("Plane Radar");

  DEBUG_LOG_HEAP("at boot");
  bootButtonInit();
  displayInit();
  DEBUG_LOG_HEAP("after display");
  // Before WiFi: the frame buffer needs 115 KB contiguous and the network stack
  // plus the reused TLS session leave a largest free block of ~9 KB. Claimed
  // here it always succeeds; claimed later it may never succeed again.
  if (!ui::radarDisplayReserveFrame()) {
    Serial.println("radar: frame buffer unavailable — falling back to direct draw");
  }
  // The 115 KB sprite is the single biggest allocation in the system; this is
  // the line to look at first if it ever starts failing.
  DEBUG_LOG_HEAP("after sprite");
  if (wifiShowsSetupScreenOnBoot()) {
    statusScreenPortal();
  }
  services::location::init();
  ui::radar::rangeInit();

  if (wifiSetupConnect()) {
    showRadarIfConnected();
  }

  // ADS-B runs on its own task from here: the fetch blocks, almost all of it
  // waiting on the socket, and loop() must keep rendering meanwhile.
  g_fetch_task_ok = services::adsb::startFetchTask();
  DEBUG_LOG("setup: fetch task %s", g_fetch_task_ok ? "started" : "FAILED");
  DEBUG_LOG_HEAP("after setup");
}

void loop() {
  handleBootButton();
  wifiLoop();

  if (WiFi.status() != WL_CONNECTED) {
    if (g_radar_visible) {
      Serial.println("WiFi lost — will reconnect");
      DEBUG_LOG_HEAP("on wifi loss");
      g_radar_visible = false;
      g_render.reset();
    }

    if (g_wifi_down_since == 0) {
      g_wifi_down_since = millis();
    }

    const unsigned long down_ms = millis() - g_wifi_down_since;
    if (down_ms >= config::kWifiDownGraceMs &&
        millis() - g_last_reconnect_ms >= config::kWifiReconnectIntervalMs) {
      g_last_reconnect_ms = millis();
      if (wifiReconnect()) {
        g_wifi_down_since = 0;
        showRadarIfConnected();
      }
    }
  } else {
    g_wifi_down_since = 0;
    // Task creation can fail under heap pressure right after the 115 KB sprite
    // is allocated; without it nothing ever fetches, so keep retrying slowly.
    if (!g_fetch_task_ok &&
        millis() - g_last_task_retry_ms >= config::kFetchTaskRetryMs) {
      g_last_task_retry_ms = millis();
      g_fetch_task_ok = services::adsb::startFetchTask();
    }
    // Portal saves happen off-screen and out of band with the render policy.
    if (wifiConsumeSettingsChanged()) {
      g_render.requestRedraw();
    }
    if (!g_radar_visible) {
      // Rate-limited like any other frame: showRadarIfConnected() can decline
      // to latch (aircraft list locked), and retrying a ~44 ms composite every
      // 10 ms would starve the loop for as long as that lasted.
      if (millis() - g_last_render_ms >= config::kRenderIntervalMs) {
        g_last_render_ms = millis();
        showRadarIfConnected();
      }
    } else if (millis() - g_last_render_ms >= config::kRenderIntervalMs) {
      // Fetching happens on its own task; loop() just animates the last list
      // forward by dead reckoning. Idle when there is nothing to animate --
      // but the frame *after* the last aircraft leaves must still be drawn,
      // or its symbol and tag stay burned on the panel until the next redraw.
      const bool traffic = services::adsb::hasTraffic();
      if (g_render.shouldRender(traffic)) {
        g_last_render_ms = millis();
        // The policy latches only on a real blit: a skipped clearing frame
        // recorded as painted would leave the last targets on screen.
        g_render.onFrameDrawn(traffic, ui::radarDisplayRefreshAircraft());
      }
    }
  }

  delay(10);
}
