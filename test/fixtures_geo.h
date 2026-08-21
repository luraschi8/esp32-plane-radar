// Ground truth from real adsb.fi responses. Each set is tied to the centre it
// was actually queried with -- dst/dir are the API's own distance (NM) and
// bearing (deg) from THAT point, so mixing sets would be incoherent.
#pragma once

struct GeoFixture { float lat, lon, dst_nm, dir_deg; };

constexpr double kMadridLat = 40.445564;
constexpr double kMadridLon = -3.698361;
constexpr GeoFixture kMadridFixtures[] = {
    {40.621445f, -3.559875f, 12.328f, 30.80f},
    {40.540375f, -3.559387f, 8.518f, 48.10f},
    {40.496990f, -3.589112f, 5.860f, 58.20f},
    {40.506266f, -3.561505f, 7.226f, 59.70f},
    {40.501180f, -3.562176f, 7.049f, 61.70f},
    {40.494686f, -3.559211f, 6.995f, 65.10f},
    {40.494989f, -3.557901f, 7.057f, 65.10f},
    {40.488636f, -3.574142f, 6.225f, 65.50f},
    {40.484882f, -3.577713f, 5.985f, 66.80f},
    {40.489815f, -3.561401f, 6.785f, 66.90f},
    {40.484142f, -3.575238f, 6.072f, 67.60f},
    {40.483351f, -3.577173f, 5.974f, 67.70f},
};
constexpr int kMadridFixtureCount = 12;

constexpr double kAmsterdamLat = 52.3676;
constexpr double kAmsterdamLon = 4.9041;
constexpr GeoFixture kAmsterdamFixtures[] = {
    {52.379740f, 5.199280f, 10.820f, 86.00f},
    {52.286403f, 4.842224f, 5.381f, 205.00f},
    {52.327560f, 4.828033f, 3.680f, 229.30f},
    {52.305164f, 4.771004f, 6.150f, 232.50f},
    {52.311687f, 4.784317f, 5.524f, 232.70f},
    {52.306435f, 4.765034f, 6.279f, 234.30f},
    {52.312949f, 4.768988f, 5.937f, 236.50f},
    {52.314835f, 4.771343f, 5.802f, 237.00f},
    {52.315269f, 4.770412f, 5.817f, 237.40f},
    {52.320442f, 4.739170f, 6.669f, 245.00f},
    {52.328218f, 4.722532f, 7.053f, 250.50f},
    {52.366241f, 4.712524f, 7.008f, 269.40f},
};
constexpr int kAmsterdamFixtureCount = 12;
