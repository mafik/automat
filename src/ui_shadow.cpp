// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
#include "ui_shadow.hpp"

#include <include/effects/SkImageFilters.h>
#include <include/utils/SkShadowUtils.h>

#include "color.hpp"
#include "renderer.hpp"
#include "textures.hpp"

namespace automat::ui {

SkPaint ShadowPaint(SkCanvas& canvas, float elevation_mm) {
  auto isize_px = canvas.getBaseLayerSize();
  auto size_px = Vec2(isize_px.width() - kCanvasMargin * 2, isize_px.height() - kCanvasMargin * 2);

  float shadow_sigma_mm = elevation_mm / 2;

  SkMatrix local_to_device = canvas.getLocalToDeviceAs3x3();
  SkMatrix device_to_local;
  (void)local_to_device.invert(&device_to_local);

  // Place some control points on the screen
  Vec2 control_points[2] = {
      Vec2{size_px.x / 2, 0},         // top
      Vec2{size_px.x / 2, size_px.y}  // bottom
  };

  // Move them into local coordinates
  device_to_local.mapPoints(SkSpan<SkPoint>(&control_points[0].sk, 2));
  // Keep the top point in place, move the bottom point down to follow the shadow
  Vec2 dst[2] = {control_points[0], control_points[1] - Vec2{0, elevation_mm}};

  SkMatrix matrix;
  if (!matrix.setPolyToPoly(SkSpan<const SkPoint>{&control_points[0].sk, 2},
                            SkSpan<const SkPoint>{&dst[0].sk, 2})) {
    matrix = SkMatrix::I();
  }

  SkPaint shadow_paint;
  sk_sp<SkImageFilter> merge[3] = {
      SkImageFilters::MatrixTransform(  // Shadow
          matrix, kFastSamplingOptions,
          SkImageFilters::DropShadowOnly(0, 0, shadow_sigma_mm, shadow_sigma_mm, "#09000c5b"_color,
                                         nullptr)),
      SkImageFilters::Blur(  // Ambient Occlusion
          elevation_mm / 10, elevation_mm / 10,
          SkImageFilters::ColorFilter(SkColorFilters::Lighting("#c9ced6"_color, "#000000"_color),
                                      nullptr)),
      nullptr,  // Actual Child
  };
  shadow_paint.setImageFilter(SkImageFilters::Merge(merge, 3));
  return shadow_paint;
}

}  // namespace automat::ui