// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <include/core/SkPaint.h>

#include "math.hh"

// Utilities for drawing things on the screen.

namespace automat {

// Configure the paint to draw a smooth gradient that shades the given rrect from top to bottom.
//
// This should be used for borders - the inner color of the SkPaint will draw artifacts.
//
// TODO: switch this from a simple conic gradient into a proper rrect-based shader.
void SetRRectShader(SkPaint& paint, const RRect& rrect, SkColor top, SkColor middle,
                    SkColor bottom);

}  // namespace automat