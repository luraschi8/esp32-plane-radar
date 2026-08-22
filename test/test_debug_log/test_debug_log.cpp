// The verbose-logging switch (include/debug_log.h), compiled ON.
//
// This suite defines PLANE_RADAR_DEBUG before including the header, so it sees
// the enabled expansion. test_settings asserts the disabled one -- the two
// cannot live in the same translation unit, because the header is #pragma once
// and the choice is made at include time.
#define PLANE_RADAR_DEBUG 1

#include <Arduino.h>
#include <unity.h>
#include <string>

#include "../mocks/mock_globals.h"
#include "debug_log.h"

void setUp() { Serial.capture = true; Serial.log.clear(); }
void tearDown() { Serial.capture = false; Serial.log.clear(); }

static bool logged(const char* needle) {
  return Serial.log.find(needle) != std::string::npos;
}

static void test_the_switch_reports_itself_as_enabled() {
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, DEBUG_LOG_ENABLED,
      "call sites guard expensive computation on this, so it must track the "
      "build flag rather than being hardcoded");
}

static void test_a_debug_line_is_emitted_and_tagged() {
  DEBUG_LOG("hello");
  TEST_ASSERT_TRUE_MESSAGE(logged("dbg: hello"),
      "the line must carry the dbg: tag so it is greppable and obviously not "
      "release output");
  TEST_ASSERT_FALSE_MESSAGE(Serial.log.empty(), "nothing was logged at all");
  TEST_ASSERT_TRUE_MESSAGE(Serial.log.back() == '\n',
      "each line must be terminated, or the next one runs into it");
}

static void test_format_arguments_are_substituted() {
  DEBUG_LOG("count %d name %s ratio %.2f", 42, "LEMD", 1.5);
  char m[192];
  snprintf(m, sizeof(m), "log was: '%s'", Serial.log.c_str());
  TEST_ASSERT_TRUE_MESSAGE(logged("dbg: count 42 name LEMD ratio 1.50"), m);
}

// A macro that expands its arguments twice turns `DEBUG_LOG("%d", ++n)` into a
// double increment -- a bug that exists only in debug builds, which is the
// worst possible place for one.
static void test_arguments_are_evaluated_exactly_once() {
  int calls = 0;
  auto bump = [&calls]() { return ++calls; };
  DEBUG_LOG("value %d", bump());
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, calls,
      "the macro must not evaluate its arguments more than once");
}

static void test_a_zero_argument_call_does_not_break_the_format() {
  DEBUG_LOG("plain line with a %% sign");
  TEST_ASSERT_TRUE_MESSAGE(logged("dbg: plain line with a % sign"),
      "the ##__VA_ARGS__ form must accept a bare format string");
}

// The heap line is the one OPS.md's release-clean check greps for
// (`strings firmware.bin | grep -c "dbg: "` must print 0). An untagged heap
// line would sit in a release image and pass that check, so the tag is a
// correctness property, not cosmetics.
static void test_the_heap_line_is_tagged_and_terminated() {
  ESP.free_heap = 1234;
  ESP.max_alloc = 567;
  DEBUG_LOG_HEAP("stage");
  char m[224];
  snprintf(m, sizeof(m), "log was: '%s'", Serial.log.c_str());
  TEST_ASSERT_TRUE_MESSAGE(Serial.log.rfind("dbg: ", 0) == 0, m);
  TEST_ASSERT_FALSE_MESSAGE(Serial.log.empty(), "nothing was logged at all");
  TEST_ASSERT_TRUE_MESSAGE(Serial.log.back() == '\n', m);
}

static void test_the_heap_line_evaluates_its_argument_once() {
  int calls = 0;
  auto tag = [&calls]() { ++calls; return "stage"; };
  DEBUG_LOG_HEAP(tag());
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, calls,
      "the heap macro must not evaluate its argument more than once");
}

// Both macros must survive being the lone body of an unbraced if. The disabled
// forms are covered in test_settings; these are the enabled ones, which is
// where a stray `if (...)` wrapper would break the following else.
static void test_both_macros_are_well_formed_statements_when_enabled() {
  int taken = 0;
  if (false) DEBUG_LOG("not this branch");
  else taken = 1;
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, taken, "DEBUG_LOG detached the else");

  taken = 0;
  if (false) DEBUG_LOG_HEAP("not this branch");
  else taken = 1;
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, taken, "DEBUG_LOG_HEAP detached the else");
}

static void test_the_heap_line_reports_both_numbers() {
  ESP.free_heap = 30856;
  ESP.max_alloc = 9204;
  DEBUG_LOG_HEAP("after parse");
  char m[224];
  snprintf(m, sizeof(m), "log was: '%s'", Serial.log.c_str());
  // Fragmentation is the failure mode on this chip, so the largest contiguous
  // block matters as much as the total and both must appear.
  TEST_ASSERT_TRUE_MESSAGE(logged("after parse"), m);
  TEST_ASSERT_TRUE_MESSAGE(logged("30856"), m);
  TEST_ASSERT_TRUE_MESSAGE(logged("9204"), m);
}

static void test_successive_lines_stay_separate() {
  DEBUG_LOG("first");
  DEBUG_LOG("second");
  TEST_ASSERT_TRUE(logged("dbg: first\ndbg: second\n"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_switch_reports_itself_as_enabled);
  RUN_TEST(test_a_debug_line_is_emitted_and_tagged);
  RUN_TEST(test_format_arguments_are_substituted);
  RUN_TEST(test_arguments_are_evaluated_exactly_once);
  RUN_TEST(test_a_zero_argument_call_does_not_break_the_format);
  RUN_TEST(test_the_heap_line_is_tagged_and_terminated);
  RUN_TEST(test_the_heap_line_evaluates_its_argument_once);
  RUN_TEST(test_both_macros_are_well_formed_statements_when_enabled);
  RUN_TEST(test_the_heap_line_reports_both_numbers);
  RUN_TEST(test_successive_lines_stay_separate);
  return UNITY_END();
}
