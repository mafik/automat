// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
#include "ui_rocker.hpp"

#include <include/core/SkBlendMode.h>
#include <include/core/SkBlurTypes.h>
#include <include/core/SkM44.h>
#include <include/core/SkMaskFilter.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPathBuilder.h>
#include <include/core/SkPathTypes.h>
#include <include/core/SkShader.h>
#include <include/core/SkTileMode.h>
#include <include/effects/SkGradient.h>

#include "color.hpp"
#include "ui_shape_widget.hpp"

using namespace std;

namespace automat::ui {

Rocker::Rocker(Widget* parent) : Widget(parent), clickable(*this) {
  auto icon_color = "#041f23cc"_color;
  on_icon = MakeShapeWidget(
      this, SkPath::RRect(RRect::MakeSimple(Rect::MakeAtZero(1.5_mm, 7_mm), 0.75_mm)), icon_color);
  off_icon = MakeShapeWidget(
      this,
      SkPathBuilder().addCircle(0, 0, 4_mm).addCircle(0, 0, 2.5_mm, SkPathDirection::kCCW).detach(),
      icon_color);
  layers.OrderInside(on_icon.get());
  layers.OrderInside(off_icon.get());
}

void Rocker::SetOn(bool value) {
  if (on == value) {
    return;
  }
  on = value;
  WakeAnimation();
}

Widget::Tock Rocker::Tick(time::Timer& timer) {
  Tock tock = clickable.Tick(timer);
  tock.drawing |= state.SpringTowards(on, timer.d, 0.2, 0.05);

  float t = state;
  float h = clickable.highlight * 10;

  colors[TOP_HORIZON] = color::AdjustLightness(kColor, h - 20);
  colors[TOP_FRONT_EDGE] = color::AdjustLightness(kColor, h);
  colors[FRONT_TOP_EDGE] = colors[FRONT_UPPER_CURVE] =
      color::AdjustLightness(kColor, h + std::lerp(-20, -10, t));
  colors[FRONT_LOWER_CURVE] = colors[FRONT_BOTTOM_EDGE] =
      color::AdjustLightness(kColor, h + std::lerp(-10, 0, t));
  colors[BOTTOM_FRONT_EDGE] = color::AdjustLightness(kColor, h - 20);
  colors[BOTTOM_HORIZON] = color::AdjustLightness(kColor, h - 50);

  positions[TOP_HORIZON] = 0.0f;
  positions[TOP_FRONT_EDGE] = std::lerp(0.15f, 0.02f, t);
  positions[FRONT_TOP_EDGE] = std::lerp(0.2f, 0.04f, t);
  positions[FRONT_UPPER_CURVE] = 0.4f;
  positions[FRONT_LOWER_CURVE] = 0.6f;
  positions[FRONT_BOTTOM_EDGE] = std::lerp(0.96f, 0.8f, t);
  positions[BOTTOM_FRONT_EDGE] = std::lerp(0.98f, 0.85f, t);
  positions[BOTTOM_HORIZON] = 1.0f;

  // Chosen so that an icon is rectangular in its "active" state
  float icon_w = kBounds.rect.Height() * 0.36f - 1_mm;
  auto on_rect =
      Rect(kBounds.rect.CenterX() - icon_w / 2,
           std::lerp(kBounds.rect.top, kBounds.rect.bottom, std::lerp(0.5f, 0.4f, t)),
           kBounds.rect.CenterX() + icon_w / 2,
           std::lerp(kBounds.rect.top, kBounds.rect.bottom, positions[FRONT_TOP_EDGE]) - 1_mm);
  auto off_rect =
      Rect(kBounds.rect.CenterX() - icon_w / 2,
           std::lerp(kBounds.rect.top, kBounds.rect.bottom, positions[FRONT_BOTTOM_EDGE]) + 1_mm,
           kBounds.rect.CenterX() + icon_w / 2,
           std::lerp(kBounds.rect.top, kBounds.rect.bottom, std::lerp(0.6f, 0.5f, t)));
  on_icon->local_to_parent =
      SkM44(SkMatrix::RectToRectOrIdentity(Rect::MakeAtZero(8_mm, 8_mm), on_rect));
  off_icon->local_to_parent =
      SkM44(SkMatrix::RectToRectOrIdentity(Rect::MakeAtZero(8_mm, 8_mm), off_rect));

  return tock;
}

void Rocker::Draw(SkCanvas& canvas) const {
  if (state > 0) {
    SkPaint shadow;
    shadow.setColor("#080c13"_color);
    shadow.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 1_mm * state));
    auto shadow_rect = kBounds.rect;
    shadow_rect.top = shadow_rect.bottom + 2_mm;
    shadow_rect.left += 1_mm;
    shadow_rect.right -= 1_mm;
    shadow_rect = shadow_rect.MoveBy(Vec2(0, 1_mm * (1 - state)));
    canvas.drawRect(shadow_rect, shadow);
  }

  SkPaint face_paint;
  SkPoint face_pts[] = {kBounds.rect.TopCenter(), kBounds.rect.BottomCenter()};
  face_paint.setShader(SkShaders::LinearGradient(
      face_pts, SkGradient{SkGradient::Colors{colors, positions, SkTileMode::kClamp}, {}}));
  canvas.drawRRect(kBounds, face_paint);

  SkPaint side_shadow;
  SkPoint side_pts[] = {kBounds.rect.LeftCenter(), kBounds.rect.RightCenter()};
  SkColor4f side_colors[4];
  side_colors[0] = side_colors[3] = color::AdjustLightness(kColor, clickable.highlight * 10 - 20);
  side_colors[1] = side_colors[2] = side_colors[0].withAlpha(0);
  float side_positions[] = {0, 0.04f, 0.96f, 1.0f};
  side_shadow.setShader(SkShaders::LinearGradient(
      side_pts,
      SkGradient{SkGradient::Colors{side_colors, side_positions, SkTileMode::kClamp}, {}}));
  canvas.drawRRect(kBounds, side_shadow);

  BakeChildren(canvas);
}

SkPath Rocker::Shape() const { return SkPath::RRect(kBounds); }

}  // namespace automat::ui
