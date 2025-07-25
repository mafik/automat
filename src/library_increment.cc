// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "library_increment.hh"

#include <include/core/SkRRect.h>
#include <include/effects/SkGradientShader.h>

#include <tracy/Tracy.hpp>

#include "base.hh"
#include "color.hh"
#include "font.hh"
#include "gui_constants.hh"
#include "library_number.hh"

using namespace automat::gui;

namespace automat::library {

Argument Increment::target_arg =
    Argument("target", Argument::kRequiresConcreteType).RequireInstanceOf<Number>();

string_view Increment::Name() const { return "Increment"; }

Ptr<Object> Increment::Clone() const { return MakePtr<Increment>(); }

void Increment::OnRun(Location& h, RunTask&) {
  ZoneScopedN("Increment");
  auto integer = target_arg.GetTyped<Number>(h);
  if (!integer.ok) {
    return;
  }
  integer.typed->value += 1;
  integer.location->ScheduleUpdate();
}

namespace {

constexpr float kHeight = 0.008;

constexpr SkColor kFontColor = 0xff03290d;

constexpr SkColor kBackgroundColor = 0xff9be0ad;
SkColor kBackgroundTopColor = color::AdjustLightness(kBackgroundColor, 5);
SkColor kBackgroundBottomColor = color::AdjustLightness(kBackgroundColor, -5);

SkColor kBorderTopColor = color::AdjustLightness(kBackgroundTopColor, 5);
SkColor kBorderBottomColor = color::AdjustLightness(kBackgroundBottomColor, -5);

SkRRect GetShape() {
  static SkRRect shape = []() -> SkRRect {
    auto& font = gui::GetFont();
    float text_width = font.MeasureText("x+1");
    float width = text_width + 2 * kMargin;
    float rad = kHeight / 2;
    return SkRRect::MakeRectXY(SkRect::MakeWH(width, kHeight), rad, rad);
  }();
  return shape;
}

SkColor GetBackgroundColor() { return 0xff9be0ad; }

SkPaint GetBackgroundPaint() {
  SkColor color = GetBackgroundColor();
  SkPaint paint;
  SkPoint pts[2] = {{0, kHeight}, {0, 0}};
  SkColor colors[2] = {kBackgroundTopColor, kBackgroundBottomColor};
  sk_sp<SkShader> gradient =
      SkGradientShader::MakeLinear(pts, colors, nullptr, 2, SkTileMode::kClamp);
  paint.setShader(gradient);
  return paint;
}

SkPaint GetBorderPaint() {
  SkColor color = GetBackgroundColor();
  SkPaint paint;
  SkPoint pts[2] = {{0, kHeight}, {0, 0}};
  SkColor colors[2] = {kBorderTopColor, kBorderBottomColor};
  sk_sp<SkShader> gradient =
      SkGradientShader::MakeLinear(pts, colors, nullptr, 2, SkTileMode::kClamp);
  paint.setShader(gradient);
  paint.setStyle(SkPaint::kStroke_Style);
  paint.setStrokeWidth(kBorderWidth);
  paint.setAntiAlias(true);
  return paint;
}

void DrawBackground(SkCanvas& canvas) {
  auto shape = GetShape();
  shape.inset(kBorderWidth / 2, kBorderWidth / 2);
  canvas.drawRRect(shape, GetBackgroundPaint());
  canvas.drawRRect(shape, GetBorderPaint());
}

}  // namespace

void Increment::Draw(SkCanvas& canvas) const {
  DrawBackground(canvas);
  SkPaint paint;
  paint.setColor(kFontColor);
  canvas.save();
  canvas.translate(kMargin, kHeight / 2 - gui::kLetterSize / 2);
  gui::GetFont().DrawText(canvas, "x+1", paint);
  canvas.restore();
}

SkPath Increment::Shape() const { return SkPath::RRect(GetShape()); }

}  // namespace automat::library