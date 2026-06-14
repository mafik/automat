// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#include "ui_enum_knob_widget.hpp"

#include <include/core/SkBlurTypes.h>
#include <include/core/SkCanvas.h>
#include <include/core/SkMaskFilter.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPath.h>
#include <include/core/SkRRect.h>
#include <include/core/SkShader.h>
#include <include/core/SkTileMode.h>
#include <include/effects/SkGradient.h>

#include <cmath>

#include "color.hpp"

namespace automat::ui {

EnumKnobWidget::EnumKnobWidget(ui::Widget* parent, int n_options)
    : ui::Widget(parent), n_options(n_options) {
  knob.unit_angle = 60_deg;
  knob.unit_distance = kGaugeRadius * 2;

  // Add some initial history to make the initial direction more stable.
  constexpr static int kInitialHistory = 40;
  for (int i = 0; i < kInitialHistory; ++i) {
    float a = i * M_PI * 2 / kInitialHistory;
    float x = knob.unit_distance * 2 * (kInitialHistory - i) / kInitialHistory;
    float amp = kGaugeRadius * 0.25;
    float perp = sinf(a * 2) * amp;
    knob.history.push_back({-x - perp, -x + perp});
  }
  knob.Update({0, 0});
  knob.value = 0;
}

SkPath EnumKnobWidget::Shape() const { return SkPath::Circle(0, 0, kGaugeRadius); }

ui::Tick EnumKnobWidget::Tock(time::Timer& timer) {
  Tick tick;
  value = KnobGet();
  auto old_value = value;
  if (std::isnan(knob.value) || std::isinf(knob.value)) {
    knob.value = 0;
  }
  while (knob.value >= 0.5) {
    knob.value -= 1;
    if (value >= n_options - 1) {
      value = 0;
    } else {
      value = value + 1;
    }
  }
  while (knob.value < -0.5) {
    knob.value += 1;
    if (value <= 0) {
      value = n_options - 1;
    } else {
      value = value - 1;
    }
  }
  if (value != old_value) {
    KnobSet(value);
  }
  tick.drawing |= click_wiggle.SpringTowards(0, timer.d, time::ToSeconds(kClickWigglePeriod),
                                             time::ToSeconds(kClickWiggleHalfTime));
  cond_code_float = (float)value + knob.value + click_wiggle.value;

  return tick;
}

void EnumKnobWidget::DrawKnobBackground(SkCanvas& canvas, int value) const {
  SkPaint white_paint;
  white_paint.setColor("#ffffff"_color);
  canvas.drawCircle(Vec2(), kRegionEndRadius, white_paint);
}

void EnumKnobWidget::Draw(SkCanvas& canvas) const {
  float cond_code_floor = floorf(cond_code_float);
  float cond_code_ceil = ceilf(cond_code_float);
  float cond_code_t = cond_code_float - cond_code_floor;  // how far towards ceil we are currently
  if (cond_code_floor < 0) {
    cond_code_floor = n_options - 1;
  }
  if (cond_code_ceil >= n_options) {
    cond_code_ceil = 0;
  }

  DrawKnobBackground(canvas, roundf(cond_code_float));

  canvas.save();
  SkRRect clip = SkRRect::MakeOval(kInnerOval.sk);
  canvas.clipRRect(clip);
  float radius = std::clamp(knob.radius, kGaugeRadius * 2, kGaugeRadius * 9);
  Vec2 delta;
  Vec2 center;
  float angle;
  if (std::isinf(radius)) {
    delta = Vec2::Polar(knob.tangent, kGaugeRadius * 2);
    canvas.translate(delta.x * cond_code_t, delta.y * cond_code_t);
  } else {
    center = Vec2::Polar(knob.tangent - 90_deg, radius);
    angle = asinf(kGaugeRadius / radius) * 2 * 180 / M_PI;
    canvas.rotate(-angle * cond_code_t, center.x, center.y);
  }

  DrawKnobSymbol(canvas, cond_code_floor);
  if (cond_code_ceil != cond_code_floor) {
    if (std::isinf(radius)) {
      canvas.translate(-delta.x, -delta.y);
    } else {
      canvas.rotate(angle, center.x, center.y);
    }
    DrawKnobSymbol(canvas, cond_code_ceil);
  }
  canvas.restore();

  DrawKnobBelowGlass(canvas);

  if constexpr (kDebugKnob) {
    SkPaint circle_paint;
    circle_paint.setColor("#ff0000"_color);
    circle_paint.setStyle(SkPaint::kStroke_Style);
    if (std::isinf(knob.radius)) {  // line
      Vec2 a = Vec2::Polar(knob.tangent, -10_mm);
      Vec2 b = Vec2::Polar(knob.tangent, 10_mm);
      canvas.drawLine(a.sk, b.sk, circle_paint);
    } else {
      canvas.drawCircle(knob.center, knob.radius, circle_paint);
    }
    SkPaint history_paint;
    history_paint.setColor("#00ff00"_color);
    for (auto& point : knob.history) {
      canvas.drawCircle(point, 0.1_mm, history_paint);
    }
  }

  {    // Glass effects
    {  // shadow
      SkPaint paint;
      paint.setColor("#00000080"_color);
      paint.setMaskFilter(SkMaskFilter::MakeBlur(SkBlurStyle::kNormal_SkBlurStyle, kBorderWidth));
      canvas.save();
      RRect clip = RRect::MakeSimple(kGaugeOval, kGaugeRadius);
      canvas.clipRRect(clip.sk);
      SkPath path = SkPath::Circle(0, -kBorderWidth * 2, kGaugeRadius).makeToggleInverseFillType();
      canvas.drawPath(path, paint);
      canvas.restore();
    }

    {  // sky reflection
      SkPaint paint;
      SkColor4f colors[] = {"#ffffffaa"_color4f, "#ffffff30"_color4f, "#ffffff00"_color4f};
      paint.setShader(SkShaders::RadialGradient(
          Vec2(0, kMiddleR), kGaugeRadius * 1.5,
          SkGradient{SkGradient::Colors{colors, SkTileMode::kClamp}, {}}));
      canvas.save();
      RRect clip = RRect::MakeSimple(kInnerOval, kInnerRadius);
      canvas.clipRRect(clip.sk);
      canvas.drawCircle(Vec2(0, kGaugeRadius * 2), hypotf(kGaugeRadius * 2, kGaugeRadius), paint);
      canvas.restore();
    }

    {  // light edge
      SkPaint paint;
      SkPoint pts[] = {Vec2(-kGaugeRadius, 0), Vec2(kGaugeRadius, 0)};
      SkColor4f colors[] = {"#ffffff20"_color4f, "#ffffffaa"_color4f, "#ffffff20"_color4f};
      paint.setShader(SkShaders::LinearGradient(
          pts, SkGradient{SkGradient::Colors{colors, SkTileMode::kClamp}, {}}));
      paint.setStyle(SkPaint::kStroke_Style);
      paint.setStrokeWidth(kBorderWidth);
      paint.setMaskFilter(
          SkMaskFilter::MakeBlur(SkBlurStyle::kNormal_SkBlurStyle, kBorderHalf / 3));
      canvas.drawCircle(0, 0, kGaugeRadius - kBorderHalf, paint);
    }
  }

  DrawKnobOverGlass(canvas);
}

Optional<Rect> EnumKnobWidget::TextureBounds() const {
  if (kDebugKnob || is_dragging) {
    return std::nullopt;
  }
  auto bounds = kGaugeOval;
  bounds.left -= 2_mm;
  return bounds;
}

EnumKnobWidget::ChangeEnumKnobAction::ChangeEnumKnobAction(ui::Pointer& pointer,
                                                           EnumKnobWidget& enum_knob_widget)
    : Action(pointer),
      widget(&enum_knob_widget),
      scroll_icon(pointer, ui::Pointer::kIconAllScroll) {
  if (widget) {
    widget->is_dragging = true;
    auto& history = widget->knob.history;
    if (!history.empty()) {
      Vec2 pos = pointer.PositionWithin(*widget);
      Vec2 shift = pos - history.back();
      for (auto& point : history) {
        point += shift;
      }
    }
    widget->click_wiggle.velocity += 5;
    start_time = time::SteadyNow();
    widget->WakeAnimation();
  }
}

void EnumKnobWidget::ChangeEnumKnobAction::Update() {
  if (!widget) {
    pointer.ReplaceAction(*this, nullptr);
    return;
  }
  Vec2 pos = pointer.PositionWithin(*widget);
  widget->knob.Update(pos);
  widget->WakeAnimation();
}

EnumKnobWidget::ChangeEnumKnobAction::~ChangeEnumKnobAction() {
  if (!widget) {
    return;
  }
  widget->click_wiggle.value += widget->knob.value;
  widget->knob.value = 0;

  if ((time::SteadyNow() - start_time) < kClickWigglePeriod / 2) {
    widget->knob.value -= 1;
    widget->click_wiggle.value += 1;
  }
  widget->is_dragging = false;
  widget->WakeAnimation();
}

std::unique_ptr<Action> EnumKnobWidget::FindAction(ui::Pointer& pointer,
                                                   ui::ActionTrigger trigger) {
  if (trigger == ui::PointerButton::Left) {
    return std::make_unique<ChangeEnumKnobAction>(pointer, *this);
  }
  return nullptr;
}

}  // namespace automat::ui
