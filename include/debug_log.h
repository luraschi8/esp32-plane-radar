#pragma once

/**
 * Opt-in verbose logging, compiled out entirely by default.
 *
 * Enable with `-D PLANE_RADAR_DEBUG=1` (see the `supermini-debug` env in
 * platformio.ini). When it is off, every call below expands to a `do {} while
 * (0)`: no code, no branch, and -- the part that matters on this chip -- none
 * of the format strings are emitted into flash. `sizeof(x)` style tricks are
 * deliberately avoided so unused-variable warnings do not appear either; call
 * sites must not rely on arguments being evaluated.
 *
 * Rules for call sites:
 *  - Never put a side effect in a DEBUG_LOG argument. It will not run in a
 *    release build.
 *  - Never call it from the fetch task's inner loop or the per-aircraft draw
 *    loop; a 115200-baud line is ~90 us per character and will visibly stall a
 *    43.8 ms frame.
 *  - Anything expensive to COMPUTE for the message must be guarded with
 *    DEBUG_LOG_ENABLED, not just wrapped in DEBUG_LOG.
 */

#ifndef PLANE_RADAR_DEBUG
#define PLANE_RADAR_DEBUG 0
#endif

#if PLANE_RADAR_DEBUG

#include <Arduino.h>

#define DEBUG_LOG_ENABLED 1
/** Verbose line, tagged so it is greppable and obviously not release output. */
#define DEBUG_LOG(fmt, ...) Serial.printf("dbg: " fmt "\n", ##__VA_ARGS__)
/** Free heap and largest contiguous block -- the two numbers that matter here. */
#define DEBUG_LOG_HEAP(what)                                     \
  Serial.printf("dbg: heap %-14s free %6u  largest %6u\n", what, \
                static_cast<unsigned>(ESP.getFreeHeap()),        \
                static_cast<unsigned>(ESP.getMaxAllocHeap()))

#else

#define DEBUG_LOG_ENABLED 0
#define DEBUG_LOG(fmt, ...) \
  do {                      \
  } while (0)
#define DEBUG_LOG_HEAP(what) \
  do {                       \
  } while (0)

#endif
