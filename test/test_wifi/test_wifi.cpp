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
  mockSetMs(100000);
  bootButtonInit();
  // Release the button and let the poll clear its long-press latch, then drain
  // any pending tap. A full g_gpio.reset() would orphan the ISR, which the
  // firmware attaches only once.
  g_gpio.release();
  bootButtonPollLongPress();
  while (bootButtonConsumeTap()) {}
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

// millis() is 32-bit on device but 64-bit here, so the real wrap cannot be
// reproduced. Pin the IDIOM instead: elapsed-since-start survives the wrap,
// a precomputed deadline does not. waitForLinkWithUi used the broken form.
// The real function, driven across the 49.7-day boundary. The old
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

static void test_elapsed_comparison_survives_the_millis_wrap() {
  const uint32_t start = 0xFFFFFF00u;         // ~4 s before the 49.7-day wrap
  const uint32_t window = 15000u;
  const uint32_t now = start + 100u;          // 100 ms later: NOT yet wrapped,
                                              // but start + window HAS wrapped

  const uint32_t bad_deadline = start + window;
  TEST_ASSERT_FALSE_MESSAGE(now < bad_deadline,
      "the precomputed-deadline form exits immediately across the wrap");
  TEST_ASSERT_TRUE_MESSAGE(now - start < window,
      "the elapsed form still reports 5 s of a 15 s window");
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
  RUN_TEST(test_elapsed_comparison_survives_the_millis_wrap);
  return UNITY_END();
}
