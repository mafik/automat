// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <include/core/SkPath.h>

#include "str.hh"

class SkSVGDOM;

namespace automat {

constexpr char kPlayShape[] = "M-5-8C-5.8-6-5.7 6-5 8-3 7.7 7.5 1.5 9 0 7.5-1.5-3-7.7-5-8Z";
constexpr char kNextShape[] =
    "M-7-8C-7.8-6-7.7 6-7 8-5 7.7 5.5 1.5 7 0Q7-4 6-7.5L8-8Q9-4 9 0 9 4 8 8L6 7.5Q7 4 7 "
    "0C5.5-1.5-5-7.7-7-8Z";
constexpr char kArrowShape[] = "M0 10l8-8 0-5-6 6V-10H-2V3l-6-6v5Z";
constexpr char kConnectionArrowShapeSVG[] = "M-13-8c-3 0-3 16 0 16 3-1 10-5 13-8-3-3-10-7-13-8z";
constexpr char kPowerSVG[] =
    "M-1-7V-4A1 1 0 001-4V-7A1 1 0 00-1-7ZM4-6A1 1 0 003-4 5 5 0 11-3-4 1 1 0 00-4-6 7 7 0 104-6";

constexpr char kPickSVG[] =
    "M0-4.5C-2.5-4.5-4.5-2.49-4.5 0-4.5 2.5-2.49 4.5 0 4.5S4.5 2.49 4.5 0 2.49-4.5 "
    "0-4.5ZM-.01-6c3.31 0 6 2.7 6 6 0 1.37-.46 2.64-1.24 3.65l.51.51a1.5 1.5 90 011.43.39L9.77 "
    "7.63a1.5 1.5 90 010 2.12 1.5 1.5 90 01-2.12 0L4.57 6.67A1.5 1.5 90 014.19 5.2L3.7 4.71C2.68 "
    "5.52 1.39 6 0 6-3.31 6-6 3.3-6 0-6-3.31-3.3-6 0-6Z";

enum SVGUnit {
  SVGUnit_Pixels96DPI,
  SVGUnit_Millimeters,
};

// Parse the given SVG path and return it as a properly scaled SkPath.
//
// Scaling assumes 96 DPI and converts coordinates to meters.
SkPath PathFromSVG(const char svg[], SVGUnit unit = SVGUnit_Pixels96DPI);

sk_sp<SkSVGDOM> SVGFromAsset(maf::StrView svg_contents);

}  // namespace automat