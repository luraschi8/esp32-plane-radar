#pragma once

/**
 * Opt-in verbose logging, compiled out entirely by default.
 *
 * Enable with `-DPLANE_RADAR_DEBUG=1` (see the `supermini-debug` env in
 * platformio.ini). When it is off, every call below expands to a `do {} while
 * (0)`: no code, no branch, and -- the part that matters on this chip -- none
 * of the format strings are emitted into flash. Arguments are NOT evaluated.
 *
 * Rules for call sites:
 *  - Never put a side effect in a DEBUG_LOG argument. It will not run in a
 *    release build.
 *  - A local computed only for a log message needs [[maybe_unused]]: the
 *    release build eliminates it, and this project can build with -Wall.
 *  - Anything expensive to COMPUTE for the message must be guarded with
 *    DEBUG_LOG_ENABLED, not just wrapped in DEBUG_LOG.
 *  - Never call it from the per-aircraft draw loop, the per-segment runway
 *    loop, or the fetch task's inner loop. `Serial` here is USB CDC (HWCDC),
 *    not a UART, so the cost is not baud rate: `HWCDC::write` BLOCKS on the TX
 *    ring for up to 100 ms when no host is draining it -- a full render tick,
 *    on a ~35 ms frame. A detached device with logging on must not stall.
 *  - In the draw path and the fetch task, keep each RENDERED line under 64
 *    characters: Print::printf formats into a 64-byte stack buffer and mallocs
 *    past it, and CLAUDE.md's rule is no heap in the draw path. Boot-time lines
 *    may be longer -- they run once, before any of that matters.
 *  - In the fetch task, remember the vsnprintf frame comes out of its ~3.6 KB
 *    of stack headroom -- the number fetchTaskStackFree() exists to watch.
 */

#ifndef PLANE_RADAR_DEBUG
#define PLANE_RADAR_DEBUG 0
#endif

#if PLANE_RADAR_DEBUG

#include <Arduino.h>

#define DEBUG_LOG_ENABLED 1
/** Verbose line, tagged so it is greppable and obviously not release output. */
#define DEBUG_LOG(fmt, ...) Serial.printf("dbg: " fmt "\n", ##__VA_ARGS__)
/**
 * Free heap and largest contiguous block -- the two numbers that matter here.
 * Routed through DEBUG_LOG so the `dbg: ` tag and the newline cannot drift
 * apart from it: OPS.md's release-clean check greps for that exact tag, so an
 * untagged line would sit in a release image and pass the check.
 */
#define DEBUG_LOG_HEAP(what)                                  \
  DEBUG_LOG("heap %-14s free %6u  largest %6u", what,         \
            static_cast<unsigned>(ESP.getFreeHeap()),         \
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
