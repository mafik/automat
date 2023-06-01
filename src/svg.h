#pragma once

#include <include/core/SkPath.h>

namespace automat {

// Parse the given SVG path and return it as a properly scaled SkPath.
//
// Scaling assumes 96 DPI and converts coordinates meters.
SkPath PathFromSVG(const char svg[]);

} // namespace automat