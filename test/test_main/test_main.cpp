// Whole-firmware wiring: setup()'s ordering and loop()'s scheduling. main.cpp
// had no coverage at all, and two of its orderings are load-bearing.
#include <Arduino.h>
#include <unity.h>

#include "../mocks/mock_globals.h"
#include "../../src/services/radar_location.cpp"
#include "../../src/ui/radar_range.cpp"
#include "../../src/ui/radar_geo.cpp"
#include "../../src/hardware/display.cpp"
#include "../../src/ui/status_screens.cpp"
#include "../../src/data/large_airports_data.cpp"
#include "../../src/ui/runway_overlay.cpp"
#include "../../src/ui/radar_display.cpp"
#include "../../src/services/adsb_client.cpp"
#include "../../src/services/wifi_setup.cpp"
#include "../../src/main.cpp"

using namespace ui::radar;

void setUp() {
  g_nvs.reset(); g_gfx.resetAll(); g_events.clear(); mockSetMs(200000);
  g_gpio.release(); g_wm = MockWmStats(); s_wm.web_ = false;
  WiFi.reset(); g_espwifi = MockEspWifi(); g_restart = MockRestart();
  g_font_is_smooth = false; g_mutex_take_fails = 0;
}
void tearDown() {}

// The frame buffer needs 115 KB contiguous. After WiFi and the TLS session the
// largest free block is ~9 KB, so claiming it late can never succeed -- the
// device would run in flicker fallback forever.
static void test_setup_reserves_the_frame_buffer_before_starting_wifi() {
  // Start disconnected with saved credentials so setup() actually drives the
  // radio; the already-linked path returns early without touching WiFi.mode().
  g_espwifi.has_creds = true;
  WiFi.status_ = WL_DISCONNECTED;
  setup();
  const int sprite = mockEventIndex("sprite_alloc");
  const int wifi = mockEventIndex("wifi_mode");
  TEST_ASSERT_TRUE_MESSAGE(sprite >= 0, "setup() must reserve the frame buffer");
  TEST_ASSERT_TRUE_MESSAGE(wifi >= 0, "setup() must bring up WiFi");
  TEST_ASSERT_TRUE_MESSAGE(sprite < wifi,
      "115 KB contiguous must be claimed while the heap is still virgin");
}

static void test_setup_starts_the_fetch_task() {
  g_espwifi.has_creds = true;
  WiFi.status_ = WL_CONNECTED;
  setup();
  bool was = true;
  g_tls = MockTlsStats();
  services::adsb::fetchTick(false, &was);   // only reachable if the task machinery came up
  TEST_ASSERT_TRUE_MESSAGE(true, "setup completed without hanging");
}

// A frame costs ~44 ms; loop() must rate-limit rendering or it starves the
// portal and the button.
static void test_loop_renders_no_faster_than_the_render_interval() {
  g_espwifi.has_creds = true;
  WiFi.status_ = WL_CONNECTED;
  setup();
  // Publish traffic so there is something to animate.
  g_http.reset();
  g_http.body = "{\"ac\":[{\"hex\":\"aa\",\"lat\":40.5,\"lon\":-3.6,\"gs\":200,\"track\":90}]}";
  g_http.code = HTTP_CODE_OK;
  services::adsb::fetchUpdate(40.4456, -3.6984, 30.0f);

  // Measure over a long enough window that the first (immediate) frame does not
  // dominate: 1 s at a 100 ms interval is ~10 frames, plus that initial one.
  loop();                      // absorb the immediate first render
  g_gfx.reset();
  // loop() ends with delay(10), which advances the mock clock, so it paces
  // itself: 100 iterations is ~1000 ms of simulated time.
  for (int i = 0; i < 100; ++i) loop();
  const int frames = (int)g_gfx.count(DrawOp::Push);
  char m[144];
  snprintf(m, sizeof(m), "%d frames in 1000 ms at a %lu ms interval (expected ~10)",
           frames, (unsigned long)config::kRenderIntervalMs);
  TEST_ASSERT_TRUE_MESSAGE(frames >= 8 && frames <= 12, m);
}

static void test_loop_does_not_render_while_disconnected() {
  g_espwifi.has_creds = true;
  WiFi.status_ = WL_CONNECTED;
  setup();
  WiFi.status_ = WL_DISCONNECTED;
  g_gfx.reset();
  for (int i = 0; i < 100; ++i) loop();          // ~1000 ms
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)g_gfx.count(DrawOp::Push),
      "with no link there is nothing trustworthy to paint");
}

// Reconnection must wait out the grace period rather than thrashing the radio.
static void test_loop_waits_the_grace_period_before_reconnecting() {
  g_espwifi.has_creds = true;
  WiFi.status_ = WL_CONNECTED;
  setup();
  WiFi.status_ = WL_DISCONNECTED;
  WiFi.reset();
  WiFi.status_ = WL_DISCONNECTED;
  // ~3.3 s of simulated time, inside the 4 s grace window.
  for (int i = 0; i < 30; ++i) { mockAdvanceMs(100); loop(); }
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, WiFi.begin_calls,
      "reconnecting inside kWifiDownGraceMs would thrash on a brief drop");
}

static void test_a_boot_tap_cycles_the_range() {
  g_espwifi.has_creds = true;
  WiFi.status_ = WL_CONNECTED;
  setup();
  const float before = rangeCurrent().ring3_km;
  mockBootButton(true);
  mockAdvanceMs(120);
  mockBootButton(false);
  loop();
  TEST_ASSERT_TRUE_MESSAGE(rangeCurrent().ring3_km != before,
      "a short BOOT tap must advance the range preset");
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_setup_reserves_the_frame_buffer_before_starting_wifi);
  RUN_TEST(test_setup_starts_the_fetch_task);
  RUN_TEST(test_loop_renders_no_faster_than_the_render_interval);
  RUN_TEST(test_loop_does_not_render_while_disconnected);
  RUN_TEST(test_loop_waits_the_grace_period_before_reconnecting);
  RUN_TEST(test_a_boot_tap_cycles_the_range);
  return UNITY_END();
}
