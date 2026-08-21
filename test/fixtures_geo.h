// Real adsb.fi response captured at the author's location (Madrid).
// dst/dir are the API's OWN distance (NM) and bearing (deg) from the query
// point, so they are independent ground truth for our projection.
#pragma once

struct GeoFixture { float lat, lon, dst_nm, dir_deg; };
constexpr double kFixtureCenterLat = 40.445564;
constexpr double kFixtureCenterLon = -3.698361;
constexpr GeoFixture kGeoFixtures[] = {
    {40.496990f, -3.589112f, 5.860f, 58.20f},
    {40.484882f, -3.577713f, 5.985f, 66.80f},
    {40.482330f, -3.577332f, 5.942f, 68.20f},
    {40.483351f, -3.577173f, 5.974f, 67.70f},
    {40.484142f, -3.575238f, 6.072f, 67.60f},
    {40.488636f, -3.574142f, 6.225f, 65.50f},
    {40.457130f, -3.573553f, 5.732f, 83.00f},
    {40.473782f, -3.571869f, 6.010f, 73.60f},
    {40.473644f, -3.570947f, 6.048f, 73.80f},
    {40.474388f, -3.568054f, 6.187f, 73.70f},
    {40.465928f, -3.566749f, 6.123f, 78.50f},
    {40.460140f, -3.565491f, 6.120f, 81.80f},
    {40.473988f, -3.563019f, 6.401f, 74.50f},
    {40.501180f, -3.562176f, 7.049f, 61.70f},
};
constexpr int kGeoFixtureCount = 14;
