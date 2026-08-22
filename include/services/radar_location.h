#pragma once

namespace services::location {

/** Load saved lat/lon from NVS, or use config defaults. Call once before WiFi setup. */
void init();

/** Factory defaults when nothing is stored (also used for portal field prefill). */
double lat();
double lon();

/**
 * Read both coordinates as one consistent pair. The portal can rewrite them
 * from the loop task while the fetch task is mid-request, and a double is two
 * stores on a 32-bit core, so readers that need both must use this.
 */
void snapshot(double* out_lat, double* out_lon);

/**
 * Parse portal strings, validate, apply to the runtime values, and persist.
 * Returns whether the coordinates were ACCEPTED, not whether they reached NVS:
 * a refused NVS write still leaves them live for this session and logs
 * "applied but NOT saved". Only invalid or out-of-range input returns false.
 */
bool saveFromStrings(const char* lat_str, const char* lon_str);

/** Clear stored coordinates (e.g. with WiFi credential reset). */
void clear();

}  // namespace services::location
