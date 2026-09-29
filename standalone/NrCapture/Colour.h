#pragma once
#include "Common.h"
namespace nr {
// Windows' SDR reference white in scRGB units (1.0 = 80 nits), or 0 if unavailable.
float DesktopWhitePoint(HMONITOR monitor);
}
