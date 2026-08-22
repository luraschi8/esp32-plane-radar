// Exercises the real wifi_setup.cpp: BOOT button semantics (an ISR-latched tap
// vs a long-press reset), the force-portal flag that survives a reboot, and
// what a credential reset actually clears.
#include <Arduino.h>
#include <unity.h>
#include <algorithm>
#include <climits>
#include <map>
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
  // Both passes are fillCircle, so they are told apart by colour: one paints
  // the background over the previous dots, the other paints the new ones.
  // Counting the total only proved "something was drawn" -- it could not see an
  // erase pass that had stopped erasing.
  // The trail is a brightness gradient, so the drawn dots are many colours and
  // the erase pass is the single most-repeated one.
  std::map<uint16_t, int> by_colour;
  for (const auto& o : g_gfx.of(DrawOp::SmoothCircle)) by_colour[o.color]++;
  int erased = 0, total = 0;
  for (const auto& kv : by_colour) { erased = std::max(erased, kv.second); total += kv.second; }
  const int drawn = total - erased;
  char m[192];
  snprintf(m, sizeof(m), "%d dots painted in %d colours, %d erased in one -- the erase "
           "pass must cover every dot of the trail", drawn, (int)by_colour.size() - 1,
           erased);
  TEST_ASSERT_TRUE_MESSAGE(by_colour.size() >= 2, m);
  TEST_ASSERT_TRUE_MESSAGE(drawn > 0 && erased >= drawn, m);
}

static void test_a_long_ssid_is_truncated_not_overrun() {
  const char* long_ssid = "AVeryLongNetworkNameThatCannotPossiblyFitOnScreen";
  // Begin() draws the text and latches s_connecting_text_drawn, so a following
  // Tick() redraws nothing: this used to reset the log AFTER Begin() and then
  // loop over zero text ops, asserting nothing at all.
  g_gfx.reset();
  statusScreenConnectingBegin(long_ssid);
  statusScreenConnectingTick();
  bool saw_ssid_line = false;
  for (const auto& o : g_gfx.of(DrawOp::Text)) {
    if (o.text.rfind("AVery", 0) == 0) saw_ssid_line = true;
  }
  TEST_ASSERT_TRUE_MESSAGE(saw_ssid_line,
      "precondition: the SSID line must actually be drawn, or this is vacuous");
  for (const auto& o : g_gfx.of(DrawOp::Text)) {
    if (o.text.rfind("AVery", 0) != 0) continue;
    TEST_ASSERT_TRUE_MESSAGE(o.w <= 220,
        "the SSID line must be truncated to fit, not drawn past the panel");
    TEST_ASSERT_TRUE_MESSAGE(o.text.size() < strlen(long_ssid),
        "and must actually be shortened");
  }
}

// ------------------------------------- status screens, smooth font ---------
// On the device these screens ARE the VLW path; everything above runs the
// bitmap fallback, so their line-height summation and vertical centring were
// only ever validated against fallback metrics.

static void test_status_screens_fit_the_panel_with_the_smooth_font() {
  g_font_is_smooth = true;
  for (auto fn : {statusScreenPortal, statusScreenConnectFailed, statusScreenWifiReset}) {
    g_gfx.reset();
    fn();
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, (int)g_gfx.count(DrawOp::FillScreen),
                                  "must still clear once");
    for (const auto& o : g_gfx.of(DrawOp::Text)) {
      char m[176];
      snprintf(m, sizeof(m), "smooth-font line '%s' at y=%d h=%d runs off the panel",
               o.text.c_str(), o.y, o.h);
      TEST_ASSERT_TRUE_MESSAGE(o.y - o.h / 2 >= 0 && o.y + o.h / 2 <= 240, m);
    }
  }
  g_font_is_smooth = false;
}

static void test_the_connecting_screen_works_with_the_smooth_font() {
  g_font_is_smooth = true;
  statusScreenConnectingBegin("SomeNetwork");
  g_gfx.reset();
  statusScreenConnectingTick();
  TEST_ASSERT_TRUE_MESSAGE(g_gfx.count(DrawOp::SmoothCircle) >= 20,
      "the spinner must erase and redraw under the smooth font too");
  g_font_is_smooth = false;
}

// ------------------------------------------ force-portal across a reboot ---

// The in-RAM flag short-circuits the NVS read, so clearing it is the only way
// to exercise what actually happens on the next boot.
static void test_the_force_portal_flag_is_read_back_from_nvs_after_a_reboot() {
  mockBootButton(true);
  bootButtonPollLongPress();
  mockAdvanceMs(config::kBootResetHoldMs + 100);
  bootButtonPollLongPress();                    // sets the flag in RAM and NVS
  g_gpio.release(); bootButtonPollLongPress();

  s_force_config_portal = false;                // simulate the reboot
  TEST_ASSERT_TRUE_MESSAGE(wifiShowsSetupScreenOnBoot(),
      "after a reset the flag must survive in NVS, or the device boots into a "
      "connect loop on credentials it just erased");
}

static void test_consuming_the_flag_clears_it_for_the_boot_after() {
  mockBootButton(true);
  bootButtonPollLongPress();
  mockAdvanceMs(config::kBootResetHoldMs + 100);
  bootButtonPollLongPress();
  g_gpio.release(); bootButtonPollLongPress();

  s_force_config_portal = false;                // reboot
  TEST_ASSERT_TRUE(consumeForceConfigPortal());
  s_force_config_portal = false;                // reboot again
  TEST_ASSERT_FALSE_MESSAGE(wifiShowsSetupScreenOnBoot(),
      "consuming the flag must clear NVS too, or the device is trapped in the "
      "setup portal forever");
}

static void test_a_clean_boot_does_not_force_the_portal() {
  s_force_config_portal = false;
  TEST_ASSERT_FALSE(wifiShowsSetupScreenOnBoot());
  TEST_ASSERT_FALSE(consumeForceConfigPortal());
}

// ----------------------------------------------------- AP-side TX power ----

// The SuperMini browns out at full TX power (upstream fix 2e2808e). The cap is
// applied in two places; the AP-side one is only reached via WiFiManager's
// callback, so it had no regression test at all.
static void test_the_ap_callback_caps_tx_power_and_shows_the_setup_screen() {
  ensureWifiManager();                          // registers the callback
  WiFi.reset();
  g_gfx.reset();
  s_wm.fireApCallback();
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, WiFi.txpower_calls,
      "TX power must be capped when the setup AP starts, or the board browns out");
  TEST_ASSERT_TRUE_MESSAGE(g_gfx.count(DrawOp::FillScreen) >= 1,
      "and the setup instructions must appear on the panel");
}

static void test_station_connect_also_caps_tx_power() {
  WiFi.reset();
  WiFi.status_ = WL_DISCONNECTED;
  g_espwifi.has_creds = true;
  tryConnectWithUi(String("net"), String("pw"), false);
  TEST_ASSERT_TRUE_MESSAGE(WiFi.txpower_calls >= 1,
      "the STA path must cap TX power on every attempt");
}

static void test_connect_retries_are_bounded() {
  WiFi.reset();
  WiFi.status_ = WL_DISCONNECTED;
  tryConnectWithUi(String("net"), String("pw"), false);
  char m[112];
  snprintf(m, sizeof(m), "%d WiFi.begin() calls for %u configured attempts",
           WiFi.begin_calls, (unsigned)config::kWifiConnectAttempts);
  TEST_ASSERT_EQUAL_INT_MESSAGE((int)config::kWifiConnectAttempts, WiFi.begin_calls, m);
}

// ------------------------------------------------ the boot decision tree ---
// wifiSetupConnect() was never called by any test. On a cold boot WiFi.status()
// is not WL_CONNECTED, so the "try the saved network" branch IS the normal path:
// stubbing it out means every boot opens the setup portal instead of connecting,
// and nothing noticed. A forced-portal boot that never opens the portal is
// unrecoverable without a reflash.

static void test_a_cold_boot_connects_to_the_saved_network() {
  s_force_config_portal = false;
  WiFi.reset();
  WiFi.status_ = WL_DISCONNECTED;          // as it is at power-on
  WiFi.link_up_on_begin = true;
  g_espwifi.has_creds = true;
  g_wm.reset = 0; g_wm.start_portal = 0;
  TEST_ASSERT_TRUE_MESSAGE(wifiSetupConnect(),
      "a cold boot with saved credentials must connect");
  TEST_ASSERT_TRUE_MESSAGE(WiFi.begin_calls > 0,
      "it has to actually try the saved network, not just read its status");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_wm.start_portal,
      "and must not open the setup portal when the saved network works");
}

static void test_a_boot_with_no_credentials_opens_the_portal() {
  s_force_config_portal = false;
  WiFi.reset();
  WiFi.status_ = WL_DISCONNECTED;
  g_espwifi.has_creds = false;
  g_wm.start_portal = 0;
  g_wm.portal_active_ticks = 2;
  g_wm.process_true_after = 1;             // the user finishes the form
  WiFi.link_up_on_begin = true;
  wifiSetupConnect();
  TEST_ASSERT_TRUE_MESSAGE(g_wm.start_portal > 0,
      "with nothing saved there is nothing to connect to -- the portal must open");
}

static void test_a_forced_portal_boot_erases_credentials_and_opens_the_portal() {
  // Arm the flag the way a long BOOT press does, then simulate the reboot.
  mockBootButton(true);
  bootButtonPollLongPress();
  mockAdvanceMs(config::kBootResetHoldMs + 100);
  bootButtonPollLongPress();
  g_gpio.release(); bootButtonPollLongPress();
  s_force_config_portal = false;

  WiFi.reset();
  WiFi.status_ = WL_CONNECTED;             // stale association from before
  g_espwifi.has_creds = true;
  g_wm.reset = 0; g_wm.erase = 0; g_wm.start_portal = 0;
  g_wm.portal_active_ticks = 2;
  g_wm.process_true_after = 1;
  wifiSetupConnect();
  TEST_ASSERT_TRUE_MESSAGE(g_wm.reset > 0 || g_wm.erase > 0,
      "a forced-portal boot must erase the credentials it was told to forget");
  TEST_ASSERT_TRUE_MESSAGE(g_wm.start_portal > 0,
      "and must open the portal -- otherwise the device is unrecoverable "
      "without a reflash");
}

// The blocking setup portal's service loop is the only thing polling BOOT while
// it is up; without it a user stuck in the portal cannot long-press out.
static void test_the_setup_portal_keeps_servicing_while_it_is_open() {
  s_force_config_portal = false;
  WiFi.reset();
  WiFi.status_ = WL_DISCONNECTED;          // or wifiSetupConnect returns early
  g_espwifi.has_creds = false;
  g_wm.process = 0;
  g_wm.portal_active_ticks = 5;
  g_wm.process_true_after = 4;
  WiFi.link_up_on_begin = true;
  wifiSetupConnect();
  char m[144];
  snprintf(m, sizeof(m), "the portal loop called process() %d times", g_wm.process);
  TEST_ASSERT_TRUE_MESSAGE(g_wm.process >= 4, m);
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
  RUN_TEST(test_status_screens_fit_the_panel_with_the_smooth_font);
  RUN_TEST(test_the_connecting_screen_works_with_the_smooth_font);
  RUN_TEST(test_the_force_portal_flag_is_read_back_from_nvs_after_a_reboot);
  RUN_TEST(test_consuming_the_flag_clears_it_for_the_boot_after);
  RUN_TEST(test_a_clean_boot_does_not_force_the_portal);
  RUN_TEST(test_a_cold_boot_connects_to_the_saved_network);
  RUN_TEST(test_a_boot_with_no_credentials_opens_the_portal);
  RUN_TEST(test_a_forced_portal_boot_erases_credentials_and_opens_the_portal);
  RUN_TEST(test_the_setup_portal_keeps_servicing_while_it_is_open);
  RUN_TEST(test_the_ap_callback_caps_tx_power_and_shows_the_setup_screen);
  RUN_TEST(test_station_connect_also_caps_tx_power);
  RUN_TEST(test_connect_retries_are_bounded);
  return UNITY_END();
}
