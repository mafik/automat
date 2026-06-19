// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
#include "ui_shadow.hpp"

#include <include/utils/SkShadowUtils.h>

#include "root_widget.hpp"
#include "textures.hpp"
#include "units.hpp"

namespace automat::ui {

ShadowWidget::ShadowWidget(Widget* parent) : Widget(parent) { parent->layers.OrderBelow(this); }

void ShadowWidget::Draw(SkCanvas& canvas) const {
  constexpr float kMinElevation = 1_mm;
  constexpr float kElevationRange = 8_mm;

  auto& rw = FindRootWidget();
  auto window_size_px = rw.size * rw.display_pixels_per_meter;
  float elevation_mm = kMinElevation + elevation * kElevationRange;
  float shadow_sigma_mm = elevation_mm / 2;

  SkMatrix local_to_device = canvas.getLocalToDeviceAs3x3();
  SkMatrix device_to_local;
  (void)local_to_device.invert(&device_to_local);

  // Place some control points on the screen
  Vec2 control_points[2] = {
      Vec2{window_size_px.width / 2, 0},                     // top
      Vec2{window_size_px.width / 2, window_size_px.height}  // bottom
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
  // Simulate shadow & ambient occlusion.
  shadow_paint.setImageFilter(SkImageFilters::Merge(
      SkImageFilters::MatrixTransform(
          matrix, kFastSamplingOptions,
          SkImageFilters::DropShadowOnly(0, 0, shadow_sigma_mm, shadow_sigma_mm, "#09000c5b"_color,
                                         nullptr)),
      SkImageFilters::Blur(
          elevation_mm / 10, elevation_mm / 10,
          SkImageFilters::ColorFilter(SkColorFilters::Lighting("#c9ced6"_color, "#000000"_color),
                                      nullptr))));
  shadow_paint.setAlphaf(alpha);
  canvas.saveLayer(nullptr, &shadow_paint);
  parent->DrawCached(canvas);
  canvas.restore();
}

}  // namespace automat::ui