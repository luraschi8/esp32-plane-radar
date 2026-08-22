// Exercises the real wifi_setup.cpp: BOOT button semantics (an ISR-latched tap
// vs a long-press reset), the force-portal flag that survives a reboot, and
// what a credential reset actually clears.
#include <Arduino.h>
#include <unity.h>
#include <cstdint>

#include "../mocks/mock_globals.h"
#include "../../src/services/radar_location.cpp"
#include "../../src/ui/radar_range.cpp"
#include "../../src/ui/radar_geo.cpp"
#include "../../src/hardware/display.cpp"
#include "../../src/ui/status_screens.cpp"
#include "../../src/services/wifi_setup.cpp"

using namespace ui::radar;

void setUp() {
  g_nvs.reset(); g_gfx.reset(); g_wm = MockWmStats();
  g_restart = MockRestart(); WiFi.reset(); g_espwifi = MockEspWifi();
  // The WiFiManager instance is a file-static in wifi_setup.cpp, so its
  // portal-active flag survives between tests; resetting only the counters
  // would make startLanWebPortal() early-return and hide the behaviour.
  s_wm.web_ = false;
  mockSetMs(100000);
  bootButtonInit();
  // Release the button and let the poll clear its long-press latch, then drain
  // any pending tap. A full g_gpio.reset() would orphan the ISR, which the
  // firmware attaches only once.
  g_gpio.release();
  bootButtonPollLongPress();
  // Bounded: an unbounded drain spins forever if the latch is ever left set,
  // turning a clean assertion failure into a hung test run.
  for (int i = 0; i < 8 && bootButtonConsumeTap(); ++i) {}
}
void tearDown() {}

/** Press and release, holding for the given time. */
static void pressFor(unsigned long ms) {
  mockBootButton(true);
  mockAdvanceMs(ms);
  mockBootButton(false);
}

// ------------------------------------------------------------- taps -------

static void test_a_normal_tap_is_latched() {
  pressFor(120);
  TEST_ASSERT_TRUE_MESSAGE(bootButtonConsumeTap(), "a 120 ms press is a tap");
}

// The latch exists so a tap during a blocking fetch or redraw is not lost.
static void test_a_tap_survives_blocking_work_before_it_is_read() {
  pressFor(120);
  mockAdvanceMs(4000);                        // a long fetch happens here
  TEST_ASSERT_TRUE_MESSAGE(bootButtonConsumeTap(), "the tap must still be waiting");
}

static void test_a_tap_is_consumed_exactly_once() {
  pressFor(120);
  TEST_ASSERT_TRUE(bootButtonConsumeTap());
  TEST_ASSERT_FALSE_MESSAGE(bootButtonConsumeTap(), "one press must not cycle twice");
}

static void test_contact_bounce_below_the_debounce_is_ignored() {
  pressFor(config::kBootTapMinMs / 2);
  TEST_ASSERT_FALSE_MESSAGE(bootButtonConsumeTap(),
      "a press shorter than kBootTapMinMs is bounce, not intent");
}

static void test_multiple_taps_each_register() {
  for (int i = 0; i < 3; ++i) {
    pressFor(100);
    TEST_ASSERT_TRUE(bootButtonConsumeTap());
    mockAdvanceMs(200);
  }
}

// ------------------------------------------------------- long press -------

static void test_holding_past_the_threshold_resets_and_reboots() {
  services::location::saveFromStrings("41.0", "-4.0");
  rangeInit();
  saveMilesFromPortal("T");
  saveRunwaysFromPortal("");

  mockBootButton(true);
  bootButtonPollLongPress();                  // loop() sees the press start
  mockAdvanceMs(config::kBootResetHoldMs + 100);
  bootButtonPollLongPress();                  // and again once it has been held

  TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_restart.count, "a long hold must reboot");
  TEST_ASSERT_EQUAL_DOUBLE_MESSAGE(config::kDefaultRadarLat, services::location::lat(),
                                   "the saved location must be cleared");
  TEST_ASSERT_FALSE_MESSAGE(useMiles(), "units must return to km");
  TEST_ASSERT_TRUE_MESSAGE(showRunways(), "the runway overlay must return to ON");
}

// Documented behaviour: the range preset is deliberately NOT cleared.
static void test_a_reset_keeps_the_range_preset() {
  Preferences seed; seed.begin("planeradar", false); seed.putUChar("rangeIdx", 0); seed.end();
  rangeInit();
  rangeNext(); rangeNext();
  const float chosen = rangeCurrent().ring3_km;

  mockBootButton(true);
  bootButtonPollLongPress();
  mockAdvanceMs(config::kBootResetHoldMs + 100);
  bootButtonPollLongPress();

  rangeInit();
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(chosen, rangeCurrent().ring3_km,
      "a credential reset must leave the range preset alone");
}

static void test_a_short_hold_does_not_reset() {
  mockBootButton(true);
  bootButtonPollLongPress();
  mockAdvanceMs(config::kBootResetHoldMs / 2);
  bootButtonPollLongPress();
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_restart.count, "not held long enough");
  mockBootButton(false);
}

static void test_a_long_hold_does_not_also_register_a_tap() {
  mockBootButton(true);
  mockAdvanceMs(config::kBootResetHoldMs + 200);
  mockBootButton(false);
  TEST_ASSERT_FALSE_MESSAGE(bootButtonConsumeTap(),
      "a reset hold must not also cycle the range on release");
}

// ------------------------------------------------- force-portal flag -------

static void test_the_setup_screen_flag_survives_a_reboot() {
  TEST_ASSERT_FALSE_MESSAGE(wifiShowsSetupScreenOnBoot(), "nothing pending on a clean boot");
  mockBootButton(true);
  bootButtonPollLongPress();
  mockAdvanceMs(config::kBootResetHoldMs + 100);
  bootButtonPollLongPress();
  TEST_ASSERT_TRUE_MESSAGE(wifiShowsSetupScreenOnBoot(),
      "after a reset the next boot must go straight to the setup screen, "
      "not sit in a connect loop on credentials that were just erased");
}

static void test_button_state_is_readable_directly() {
  mockBootButton(true);
  TEST_ASSERT_TRUE(wifiBootButtonPressed());
  mockBootButton(false);
  TEST_ASSERT_FALSE(wifiBootButtonPressed());
}

static void test_the_interrupt_is_attached_only_once() {
  const int before = g_gpio.isr_attached;
  bootButtonInit(); bootButtonInit(); bootButtonInit();
  TEST_ASSERT_EQUAL_INT_MESSAGE(before, g_gpio.isr_attached,
      "re-init must not stack duplicate interrupt handlers");
}

// --------------------------------------------------- rollover safety -------

// --------------------------------------------------- rollover safety -------

// Drives the REAL function across the 49.7-day boundary. The old
// `deadline = millis() + attempt_ms` form exits instantly here, aborting every
// WiFi connect attempt for the duration of the wrap.
static void test_connect_wait_still_waits_across_the_millis_wrap() {
  WiFi.status_ = WL_DISCONNECTED;              // never links, so it runs its full budget
  mockSetMs(0xFFFFF000u);                      // ~4 s before the wrap
  const uint32_t start = millis();
  waitForLinkWithUi("net", 15000);
  const uint32_t elapsed = millis() - start;
  char m[96];
  snprintf(m, sizeof(m), "waited %u ms of a 15000 ms budget across the wrap", elapsed);
  TEST_ASSERT_TRUE_MESSAGE(elapsed >= 14000u, m);
}

// ------------------------------------------------ LAN portal lifecycle -----

static void test_the_lan_portal_starts_once_while_linked() {
  WiFi.status_ = WL_CONNECTED;
  for (int i = 0; i < 10; ++i) wifiLoop();
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_wm.start_web,
      "the portal must be started once, not restarted on every loop iteration");
  TEST_ASSERT_TRUE_MESSAGE(g_wm.process >= 10, "and serviced on every iteration");
}

static void test_the_lan_portal_stops_once_when_the_link_drops() {
  WiFi.status_ = WL_CONNECTED;
  wifiLoop();
  TEST_ASSERT_EQUAL_INT(1, g_wm.start_web);
  WiFi.status_ = WL_DISCONNECTED;
  for (int i = 0; i < 5; ++i) wifiLoop();
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_wm.stop_web,
      "stopping must be idempotent -- the same double-teardown shape that bit "
      "the TLS session");
}

static void test_the_portal_restarts_after_a_reconnect() {
  WiFi.status_ = WL_CONNECTED; wifiLoop();
  WiFi.status_ = WL_DISCONNECTED; wifiLoop();
  WiFi.status_ = WL_CONNECTED; wifiLoop();
  TEST_ASSERT_EQUAL_INT_MESSAGE(2, g_wm.start_web,
      "the portal must come back after the link returns");
}

// An associated-but-no-DHCP-lease link must not count as up.
static void test_a_link_without_an_ip_is_not_treated_as_up() {
  WiFi.status_ = WL_CONNECTED;
  WiFi.ip = IPAddress(0, 0, 0, 0);
  wifiLoop();
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_wm.start_web,
      "associated without an address is not a usable link");
}

// ------------------------------------------------- portal parameters -------

// WiFiManager overwrites each parameter with what the browser posted, and an
// unchecked box posts nothing. Without re-arming, the field renders checked
// while reading empty and can never be switched back on.
static void test_saving_params_rearms_the_checkbox_values() {
  rangeInit();
  s_param_miles.setValue("", 2);
  s_param_runways.setValue("", 2);
  onPortalParamsSaved();
  TEST_ASSERT_EQUAL_STRING_MESSAGE("T", s_param_miles.getValue(),
      "the miles checkbox must be re-armed for the next page render");
  TEST_ASSERT_EQUAL_STRING_MESSAGE("T", s_param_runways.getValue(),
      "and so must the runway checkbox");
}

static void test_saving_params_applies_a_valid_location() {
  s_param_lat.setValue("51.470000", 20);
  s_param_lon.setValue("-0.454300", 20);
  onPortalParamsSaved();
  TEST_ASSERT_DOUBLE_WITHIN(1e-4, 51.47, services::location::lat());
  TEST_ASSERT_DOUBLE_WITHIN(1e-4, -0.4543, services::location::lon());
}

static void test_saving_params_keeps_the_old_location_when_input_is_invalid() {
  services::location::saveFromStrings("40.0", "-3.0");
  s_param_lat.setValue("not-a-number", 20);
  s_param_lon.setValue("-3.0", 20);
  onPortalParamsSaved();
  TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(1e-6, 40.0, services::location::lat(),
      "a rejected coordinate must not disturb the stored one");
}

// ------------------------------------------------------ status screens -----

static int fillScreenCount() { return (int)g_gfx.count(DrawOp::FillScreen); }

static void test_each_status_screen_clears_before_drawing() {
  for (auto fn : {statusScreenPortal, statusScreenConnectFailed, statusScreenWifiReset}) {
    g_gfx.reset();
    fn();
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, fillScreenCount(),
        "a status screen must clear the panel exactly once before drawing");
    TEST_ASSERT_TRUE_MESSAGE(g_gfx.count(DrawOp::Text) > 0, "and then draw text");
  }
}

static void test_status_screen_text_fits_the_panel() {
  g_gfx.reset();
  statusScreenPortal();          // the tallest: six lines, mixed sizes
  for (const auto& o : g_gfx.of(DrawOp::Text)) {
    char m[160];
    snprintf(m, sizeof(m), "portal line '%s' at y=%d h=%d runs off the 240 px panel",
             o.text.c_str(), o.y, o.h);
    TEST_ASSERT_TRUE_MESSAGE(o.y - o.h / 2 >= 0 && o.y + o.h / 2 <= 240, m);
  }
}

static void test_the_portal_screen_shows_how_to_reach_it() {
  g_gfx.reset();
  statusScreenPortal();
  bool ap = false, host = false;
  for (const auto& o : g_gfx.of(DrawOp::Text)) {
    if (o.text == config::kPortalApName) ap = true;
    if (o.text == config::kPortalHostUrl) host = true;
  }
  TEST_ASSERT_TRUE_MESSAGE(ap, "the setup screen must name the AP to join");
  TEST_ASSERT_TRUE_MESSAGE(host, "and the address to open");
}

// Each tick must erase the dots it drew last time, or the panel accumulates
// green specks around the rim.
static void test_the_connecting_spinner_erases_what_it_drew() {
  statusScreenConnectingBegin("TestNet");
  g_gfx.reset();
  statusScreenConnectingTick();
  int erased = 0, drawn = 0;
  for (const auto& o : g_gfx.of(DrawOp::SmoothCircle)) ++drawn;
  for (const auto& o : g_gfx.of(DrawOp::Circle)) ++erased;      // fillCircle records as SmoothCircle
  // The erase pass uses fillCircle (recorded as SmoothCircle) too, so count all
  // circle ops and require at least one erase for every dot drawn.
  TEST_ASSERT_TRUE_MESSAGE(drawn >= 20,
      "a tick must erase the previous dots and draw the new ones");
  (void)erased;
}

static void test_a_long_ssid_is_truncated_not_overrun() {
  const char* long_ssid = "AVeryLongNetworkNameThatCannotPossiblyFitOnScreen";
  statusScreenConnectingBegin(long_ssid);
  g_gfx.reset();
  statusScreenConnectingTick();
  for (const auto& o : g_gfx.of(DrawOp::Text)) {
    if (o.text.rfind("AVery", 0) != 0) continue;
    TEST_ASSERT_TRUE_MESSAGE(o.w <= 220,
        "the SSID line must be truncated to fit, not drawn past the panel");
    TEST_ASSERT_TRUE_MESSAGE(o.text.size() < strlen(long_ssid),
        "and must actually be shortened");
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  // Runs first: s_force_config_portal is a file-static no test can clear.
  RUN_TEST(test_the_setup_screen_flag_survives_a_reboot);
  RUN_TEST(test_a_normal_tap_is_latched);
  RUN_TEST(test_a_tap_survives_blocking_work_before_it_is_read);
  RUN_TEST(test_a_tap_is_consumed_exactly_once);
  RUN_TEST(test_contact_bounce_below_the_debounce_is_ignored);
  RUN_TEST(test_multiple_taps_each_register);
  RUN_TEST(test_holding_past_the_threshold_resets_and_reboots);
  RUN_TEST(test_a_reset_keeps_the_range_preset);
  RUN_TEST(test_a_short_hold_does_not_reset);
  RUN_TEST(test_a_long_hold_does_not_also_register_a_tap);
  RUN_TEST(test_button_state_is_readable_directly);
  RUN_TEST(test_the_interrupt_is_attached_only_once);
  RUN_TEST(test_connect_wait_still_waits_across_the_millis_wrap);
  RUN_TEST(test_the_lan_portal_starts_once_while_linked);
  RUN_TEST(test_the_lan_portal_stops_once_when_the_link_drops);
  RUN_TEST(test_the_portal_restarts_after_a_reconnect);
  RUN_TEST(test_a_link_without_an_ip_is_not_treated_as_up);
  RUN_TEST(test_saving_params_rearms_the_checkbox_values);
  RUN_TEST(test_saving_params_applies_a_valid_location);
  RUN_TEST(test_saving_params_keeps_the_old_location_when_input_is_invalid);
  RUN_TEST(test_each_status_screen_clears_before_drawing);
  RUN_TEST(test_status_screen_text_fits_the_panel);
  RUN_TEST(test_the_portal_screen_shows_how_to_reach_it);
  RUN_TEST(test_the_connecting_spinner_erases_what_it_drew);
  RUN_TEST(test_a_long_ssid_is_truncated_not_overrun);
  return UNITY_END();
}
