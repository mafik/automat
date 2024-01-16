#pragma once

#include <include/core/SkPath.h>

namespace automat {

constexpr char kPlayShape[] = "M-5-8C-5.8-6-5.7 6-5 8-3 7.7 7.5 1.5 9 0 7.5-1.5-3-7.7-5-8Z";
constexpr char kNextShape[] =
    "M-7-8C-7.8-6-7.7 6-7 8-5 7.7 5.5 1.5 7 0Q7-4 6-7.5L8-8Q9-4 9 0 9 4 8 8L6 7.5Q7 4 7 "
    "0C5.5-1.5-5-7.7-7-8Z";
constexpr char kArrowShape[] = "M0 10l8-8 0-5-6 6V-10H-2V3l-6-6v5Z";
constexpr char kConnectionArrowShapeSVG[] = "M-13-8c-3 0-3 16 0 16 3-1 10-5 13-8-3-3-10-7-13-8z";

// Parse the given SVG path and return it as a properly scaled SkPath.
//
// Scaling assumes 96 DPI and converts coordinates to meters.
SkPath PathFromSVG(const char svg[]);

}  // namespace automat