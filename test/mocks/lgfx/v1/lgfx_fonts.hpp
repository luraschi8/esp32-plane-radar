#pragma once
#include <LovyanGFX.hpp>
namespace lgfx { namespace v1 { namespace fonts {
inline const GFXfont Font2{"Font2", 12};
inline const GFXfont FreeSans9pt7b{"FreeSans9", 13};
inline const GFXfont FreeSans12pt7b{"FreeSans12", 17};
inline const GFXfont FreeSans18pt7b{"FreeSans18", 25};
inline const GFXfont FreeSansBold9pt7b{"FreeSansBold9", 13};
inline const GFXfont FreeSansBold12pt7b{"FreeSansBold12", 17};
inline const GFXfont FreeSansBold18pt7b{"FreeSansBold18", 25};
}}}

// The real lgfx_fonts.hpp declares exactly this pair at global scope. It is
// what made `namespace fonts = lgfx::v1::fonts;` a redeclaration error and
// broke the build on LovyanGFX >= 1.2.x -- mirrored here so the mock cannot
// quietly accept code the device would reject.
namespace fonts { using namespace lgfx::v1::fonts; }
using namespace fonts;
