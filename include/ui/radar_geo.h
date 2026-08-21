#pragma once

namespace ui::radar {

/**
 * Equirectangular projection of lat/lon onto the radar disc, centred on the
 * configured radar location (services::location). North is screen up.
 *
 * Longitude degrees are scaled by cos(centre latitude): a degree of longitude
 * is only 111 km at the equator and shrinks towards the poles, so without this
 * correction everything east-west is stretched by 1/cos(lat) (~1.64x at 52 deg).
 * The approximation is accurate well beyond the widest range preset (~33 km).
 * dist_km may be null when the caller will recompute the distance anyway --
 * the sqrtf is not free on a core without an FPU.
 */
void offsetKmFromCenter(float lat, float lon, float* dx_km, float* dy_km,
                        float* dist_km);

/** Screen pixels per km at the active range preset. */
float pxPerKm();

/** Project a ground-kilometre offset from the radar centre to screen pixels. */
void kmOffsetToScreen(float dx_km, float dy_km, int* out_x, int* out_y);

/** Project lat/lon to screen pixels (unclipped; may fall outside the panel). */
void latLonToScreen(float lat, float lon, int* out_x, int* out_y);

/** Squared distance from the radar centre, in px^2 (avoids a sqrt). */
int distSqFromCenter(int x, int y);

/**
 * Walk (x1,y1) back along the segment from (x0,y0) until it is inside the
 * outer grid ring. Collapses to (x0,y0) if no point on the segment qualifies.
 */
void clipPointToOuterRing(int x0, int y0, int* x1, int* y1);

}  // namespace ui::radar
