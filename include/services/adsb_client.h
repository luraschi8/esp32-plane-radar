#pragma once

#include <cstddef>
#include <cstdint>

namespace services::adsb {

struct Aircraft {
  float lat;
  float lon;
  float nose_deg;
  float track_deg;
  float gs_knots;
  /**
   * Ground velocity split into east/north km per second, computed once per
   * fetch so the render loop can dead-reckon without any trig per frame.
   */
  float vel_e_km_s;
  float vel_n_km_s;
  /**
   * Age of this position when the API reported it (its seen_pos, seconds).
   * Positions frequently arrive several seconds stale, so dead reckoning has to
   * run from when the fix was taken, not from when we fetched it.
   */
  float pos_age_s;
  /**
   * API-computed distance from the radar centre (NM); < 0 if absent. Not read
   * by the renderer, which projects from lat/lon — it is kept deliberately as
   * the independent check that caught the missing cos(latitude) term in the
   * projection, and test_geo asserts our own distances against it. Cost is
   * 4 B per slot (512 B across both buffers) and one key in the JSON filter.
   */
  float dst_nm;
  char callsign[9];
  char type[5];
  char alt[12];
};

constexpr size_t kMaxAircraft = 64;

size_t aircraftCount();
const Aircraft* aircraftList();

/**
 * Start the background fetch task. The fetch blocks (mostly waiting
 * on the network), which froze the render loop when it ran inline; on its own
 * task it yields while blocked and the display keeps animating.
 */
bool startFetchTask();

/**
 * Take/release the read lock on the shared list. aircraftCount() and
 * aircraftList() are only valid while held. Returns false on timeout, in which
 * case the caller should skip the frame rather than draw torn data.
 */
bool aircraftLock(uint32_t timeout_ms);
void aircraftUnlock();

/** Unused stack bytes remaining in the fetch task (diagnostic). */
unsigned fetchTaskStackFree();

/** Lock-free heuristic for "is there anything to animate"; may be one fetch stale. */
bool hasTraffic();

/**
 * Dead-reckoning horizon. Shared so the drawn position (clamped to it) and the
 * stale flag (tested against it) can never be judged by different numbers.
 */
constexpr float kExtrapolationHorizonSec = 12.0f;

/**
 * Seconds since the last successful fetch, clamped so a run of failures cannot
 * extrapolate an aircraft into fiction. 0 before the first fetch.
 */
float secondsSinceUpdate();

/** Seconds since the last successful fetch, unclamped. 0 before the first. */
float secondsSinceUpdateRaw();

/**
 * True once the last successful fetch is old enough that the list must not be
 * shown at all. Without this a stalled feed leaves the panel presenting
 * minutes-old traffic as live, which is worse than showing nothing.
 */
bool dataExpired();

}  // namespace services::adsb
