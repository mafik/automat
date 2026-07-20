#pragma once
// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include <include/core/SkCanvas.h>
#include <include/core/SkImage.h>
#include <include/core/SkPath.h>

#include "fn_ref.hpp"
#include "math.hpp"

// Utilities for drawing things on the screen.

namespace automat {

// Configure the paint to draw a smooth gradient that shades the given rrect from top to bottom.
//
// This should be used for borders - the inner color of the SkPaint will draw artifacts.
//
// TODO: switch this from a simple conic gradient into a proper rrect-based shader.
void SetRRectShader(SkPaint& paint, const RRect& rrect, SkColor4f top, SkColor4f middle,
                    SkColor4f bottom);

// Helper that caches some draw commands into a fixed-resolution image with some clip path.
//
// Used to avoid compute-heavy gradients while preserving their smoothness & sharp clip boundary.
//
// High quality is achieved by:
// - using high quality bicubic filtering when zooming in
// - using mip-maps when zooming out
// - applying clip path only during the final draw
struct RasterPatch {
  mutable SkPath clip{};
  mutable sk_sp<SkImage> image{};

  // Draws once & then re-uses a cached image.
  void DrawCached(SkCanvas& canvas, Rect bounds, SkISize raster_size,
                  FnRef<SkPath(SkCanvas&)> draw_fn, SkPaint* paint = nullptr) const;
};

}  // namespace automat