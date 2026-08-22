// The render state machine, tested as sequences. Two shipped bugs lived here:
// the last aircraft staying burned on the panel when the sky emptied, and a
// composited-but-not-blitted frame being recorded as painted, which re-opened
// the first bug one commit after it was fixed.
#include <Arduino.h>
#include <unity.h>

#include "ui/render_policy.h"

using ui::RenderPolicy;

void setUp() {}
void tearDown() {}

/** Drive one tick: render if asked, report the outcome. Returns "did we draw". */
static bool tick(RenderPolicy& p, bool traffic, bool blit_succeeds = true) {
  if (!p.shouldRender(traffic)) return false;
  p.onFrameDrawn(traffic, blit_succeeds);
  return true;
}

static void test_idles_when_there_has_never_been_traffic() {
  RenderPolicy p;
  for (int i = 0; i < 20; ++i) {
    TEST_ASSERT_FALSE_MESSAGE(tick(p, false), "empty sky must not burn frames");
  }
}

static void test_renders_continuously_while_traffic_exists() {
  RenderPolicy p;
  for (int i = 0; i < 20; ++i) {
    TEST_ASSERT_TRUE(tick(p, true));
  }
}

// THE GHOST-AIRCRAFT BUG: exactly one frame after the sky empties, then idle.
static void test_exactly_one_clearing_frame_then_idle() {
  RenderPolicy p;
  tick(p, true);
  tick(p, true);
  TEST_ASSERT_TRUE_MESSAGE(tick(p, false), "the clearing frame must be drawn");
  for (int i = 0; i < 20; ++i) {
    TEST_ASSERT_FALSE_MESSAGE(tick(p, false), "only ONE clearing frame is owed");
  }
}

// THE RE-OPENED BUG: a clearing frame that was not blitted must not be recorded
// as painted, or the aircraft stay on screen forever with nothing scheduled.
static void test_declined_clearing_frame_is_retried_until_it_lands() {
  RenderPolicy p;
  tick(p, true);
  TEST_ASSERT_TRUE(tick(p, false, /*blit_succeeds=*/false));
  TEST_ASSERT_TRUE_MESSAGE(p.needsRedraw(), "a declined frame owes a retry");
  TEST_ASSERT_TRUE_MESSAGE(tick(p, false, false), "still owed after a second failure");
  TEST_ASSERT_TRUE_MESSAGE(tick(p, false, true), "and finally lands");
  TEST_ASSERT_FALSE_MESSAGE(tick(p, false), "then idles");
}

static void test_declined_frame_with_traffic_is_retried() {
  RenderPolicy p;
  TEST_ASSERT_TRUE(tick(p, true, false));
  TEST_ASSERT_TRUE_MESSAGE(p.needsRedraw(), "declined frame owes a retry");
  TEST_ASSERT_TRUE(tick(p, true, true));
  TEST_ASSERT_FALSE(p.needsRedraw());
}

// A declined draw must never leave the record claiming traffic was painted.
static void test_declined_frame_never_updates_the_record() {
  RenderPolicy p;
  tick(p, true);                       // record: traffic drawn
  TEST_ASSERT_TRUE(p.trafficDrawn());
  p.onFrameDrawn(false, /*blitted=*/false);
  TEST_ASSERT_TRUE_MESSAGE(p.trafficDrawn(),
      "a frame that never reached the panel must not clear the record");
}

// THE RANGE-TAP BUG: preset advanced, repaint declined, nothing scheduled.
// THE RANGE-TAP PATH, as main.cpp actually drives it: onRangeTap() repaints and
// reports the result. A declined repaint with an empty sky must still be owed,
// or the rings keep showing the previous preset with nothing scheduled.
static void test_a_declined_range_tap_redraw_is_retried_on_an_empty_sky() {
  RenderPolicy p;                              // no traffic, nothing ever drawn
  p.onFrameDrawn(/*traffic=*/false, /*blitted=*/false);   // repaint declined
  TEST_ASSERT_TRUE_MESSAGE(p.needsRedraw(), "the repaint is still owed");
  TEST_ASSERT_TRUE_MESSAGE(tick(p, false), "and loop() must retry it");
  TEST_ASSERT_FALSE_MESSAGE(tick(p, false), "then settle");
}

static void test_a_successful_range_tap_redraw_settles_immediately() {
  RenderPolicy p;
  p.onFrameDrawn(false, /*blitted=*/true);
  TEST_ASSERT_FALSE(p.needsRedraw());
  TEST_ASSERT_FALSE_MESSAGE(tick(p, false), "nothing further is owed");
}

static void test_reset_clears_the_record_on_connection_loss() {
  RenderPolicy p;
  tick(p, true);
  p.onFrameDrawn(false, /*blitted=*/false);   // a declined repaint
  p.reset();
  TEST_ASSERT_FALSE(p.trafficDrawn());
  TEST_ASSERT_FALSE(p.needsRedraw());
  TEST_ASSERT_FALSE_MESSAGE(tick(p, false),
      "after a reset nothing is owed: the reconnect path repaints from scratch");
}

// Traffic appearing, vanishing and reappearing must not accumulate owed frames.
static void test_repeated_cycles_do_not_leak_owed_frames() {
  RenderPolicy p;
  for (int cycle = 0; cycle < 5; ++cycle) {
    for (int i = 0; i < 3; ++i) TEST_ASSERT_TRUE(tick(p, true));
    TEST_ASSERT_TRUE_MESSAGE(tick(p, false), "one clearing frame per cycle");
    TEST_ASSERT_FALSE_MESSAGE(tick(p, false), "and no more");
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_idles_when_there_has_never_been_traffic);
  RUN_TEST(test_renders_continuously_while_traffic_exists);
  RUN_TEST(test_exactly_one_clearing_frame_then_idle);
  RUN_TEST(test_declined_clearing_frame_is_retried_until_it_lands);
  RUN_TEST(test_declined_frame_with_traffic_is_retried);
  RUN_TEST(test_declined_frame_never_updates_the_record);
  RUN_TEST(test_a_declined_range_tap_redraw_is_retried_on_an_empty_sky);
  RUN_TEST(test_a_successful_range_tap_redraw_settles_immediately);
  RUN_TEST(test_reset_clears_the_record_on_connection_loss);
  RUN_TEST(test_repeated_cycles_do_not_leak_owed_frames);
  return UNITY_END();
}
