/**
 * Plane Radar — WiFi setup, then radar UI on the round GC9A01 display.
 */

#include <Arduino.h>
#include <WiFi.h>

#include "config.h"
#include "hardware/display.h"
#include "services/adsb_client.h"
#include "services/radar_location.h"
#include "services/wifi_setup.h"
#include "ui/radar_display.h"
#include "ui/radar_range.h"
#include "ui/status_screens.h"

namespace {

bool g_radar_visible = false;
unsigned long g_wifi_down_since = 0;
unsigned long g_last_reconnect_ms = 0;
unsigned long g_last_render_ms = 0;
/** Did the last painted frame contain traffic? Drives the one clearing redraw. */
bool g_traffic_was_drawn = false;
bool g_fetch_task_ok = false;
unsigned long g_last_task_retry_ms = 0;

void showRadarIfConnected() {
  if (WiFi.status() != WL_CONNECTED) {
    g_radar_visible = false;
    return;
  }
  ui::radarDisplayDraw();
  g_traffic_was_drawn = services::adsb::hasTraffic();
  g_radar_visible = true;
}

void onRangeTap() {
  ui::radar::rangeNext();
  char range_label[12];
  ui::radar::formatCurrentRing3Label(range_label, sizeof(range_label));
  Serial.printf("Range: %s (outer ~%.0f km)\n", range_label,
                ui::radar::rangeCurrent().outer_km);

  if (g_radar_visible && WiFi.status() == WL_CONNECTED) {
    ui::radarDisplayDraw();
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

  bootButtonInit();
  displayInit();
  if (wifiShowsSetupScreenOnBoot()) {
    statusScreenPortal();
  }
  services::location::init();
  ui::radar::rangeInit();

  if (wifiSetupConnect()) {
    showRadarIfConnected();
  }

  // ADS-B runs on its own task from here: the fetch blocks for ~1.6 s, almost
  // all of it waiting on the socket, and loop() must keep rendering meanwhile.
  g_fetch_task_ok = services::adsb::startFetchTask();
}

void loop() {
  handleBootButton();
  wifiLoop();

  if (WiFi.status() != WL_CONNECTED) {
    if (g_radar_visible) {
      Serial.println("WiFi lost — will reconnect");
      g_radar_visible = false;
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
    if (!g_radar_visible) {
      showRadarIfConnected();
    } else if (millis() - g_last_render_ms >= config::kRenderIntervalMs) {
      // Fetching happens on its own task; loop() just animates the last list
      // forward by dead reckoning. Idle when there is nothing to animate --
      // but the frame *after* the last aircraft leaves must still be drawn,
      // or its symbol and tag stay burned on the panel until the next redraw.
      const bool traffic = services::adsb::hasTraffic();
      if (traffic || g_traffic_was_drawn) {
        g_last_render_ms = millis();
        ui::radarDisplayRefreshAircraft();
        g_traffic_was_drawn = traffic;
      }
    }
  }

  delay(10);
}
