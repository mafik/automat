// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "library_flip_flop.hh"

#include <include/core/SkBlurTypes.h>
#include <include/core/SkColor.h>
#include <include/core/SkMaskFilter.h>
#include <include/core/SkPaint.h>
#include <include/core/SkSamplingOptions.h>
#include <include/core/SkShader.h>
#include <include/effects/SkGradientShader.h>
#include <include/pathops/SkPathOps.h>

#include <tracy/Tracy.hpp>

#include "../build/generated/embedded.hh"
#include "animation.hh"
#include "arcline.hh"
#include "color.hh"
#include "sincos.hh"
#include "textures.hh"
#include "ui_button.hh"
#include "widget.hh"

using namespace std;

namespace automat::library {

constexpr float kYingYangRadius = 1.2_cm / 2 - 1_mm;
constexpr float kYingYangRadiusSmall = kYingYangRadius / 2;
constexpr float kYingYangButtonRadius = kYingYangRadius + 0.5_mm;
constexpr float kFlipFlopWidth = 1.8_cm;

static PersistentImage& FlipFlopColor() {
  static auto flip_flop_color = PersistentImage::MakeFromAsset(
      embedded::assets_flip_flop_color_webp, {.width = kFlipFlopWidth});
  return flip_flop_color;
}

Rect FlipFlopRect() {
  return Rect::MakeCornerZero(FlipFlopColor().width(), FlipFlopColor().height());
}

FlipFlop::FlipFlop() {}
string_view FlipFlop::Name() const { return "Flip-Flop"; }

Ptr<Object> FlipFlop::Clone() const {
  auto ret = MAKE_PTR(FlipFlop);
  ret->current_state = current_state;
  return ret;
}

void FlipFlop::Flip::OnRun(std::unique_ptr<RunTask>& task) {
  ZoneScopedN("FlipFlop");
  GetFlipFlop().Toggle();
}

void FlipFlop::OnTurnOn() {
  current_state = true;
  WakeWidgetsAnimation();
}

void FlipFlop::OnTurnOff() {
  current_state = false;
  WakeWidgetsAnimation();
}

void FlipFlop::SerializeState(ObjectSerializer& writer) const {
  writer.Key("on");
  writer.Bool(current_state);
}

bool FlipFlop::DeserializeKey(ObjectDeserializer& d, StrView key) {
  if (key == "on") {
    Status status;
    d.Get(current_state, status);
    if (!OK(status)) {
      ReportError(status.ToStr());
    }
    return true;
  }
  return false;
}

struct YingYangIcon : ui::Widget, ui::PaintMixin {
  YingYangIcon(ui::Widget* parent) : ui::Widget(parent) {}
  void Draw(SkCanvas& canvas) const override {
    ArcLine tear = ArcLine(Vec2(0, kYingYangRadius), 0_deg);
    tear.TurnConvex(180_deg, -kYingYangRadius);
    tear.TurnConvex(180_deg, -kYingYangRadiusSmall);
    tear.TurnConvex(180_deg, kYingYangRadiusSmall);
    auto black_path = tear.ToPath();
    black_path.addCircle(0, kYingYangRadiusSmall, kYingYangRadiusSmall / 4);
    black_path.addCircle(0, -kYingYangRadiusSmall, kYingYangRadiusSmall / 4);
    canvas.drawPath(black_path, paint);
  }
  SkPath Shape() const override { return SkPath::Circle(0, 0, kYingYangRadius); }
  bool CenteredAtZero() const override { return true; }
};

struct YingYangButton : ui::ColoredButton {
  YingYangButton(ui::Widget* parent, SkColor fg, SkColor bg)
      : ColoredButton(parent,
                      ui::ColoredButtonArgs{.fg = fg, .bg = bg, .radius = kYingYangButtonRadius}) {
    child = make_unique<YingYangIcon>(this);
    UpdateChildTransform();
  }
};

struct FlipFlopButton : ui::ToggleButton {
  WeakPtr<FlipFlop> flip_flop;

  FlipFlopButton(ui::Widget* parent, WeakPtr<FlipFlop> flip_flop)
      : ui::ToggleButton(parent), flip_flop(flip_flop) {
    on = make_unique<YingYangButton>(this, "#eae9e8"_color, "#1d1d1d"_color);
    off = make_unique<YingYangButton>(this, "#1d1d1d"_color, "#eae9e8"_color);
    static_cast<YingYangButton*>(this->off.get())->on_click =
        static_cast<YingYangButton*>(this->on.get())->on_click = [this](ui::Pointer&) {
          if (auto flip_flop_ptr = this->flip_flop.Lock()) {
            if (auto h = flip_flop_ptr->here) {
              h->ScheduleRun();
            }
          }
        };
  }
  bool Filled() const override {
    if (auto flip_flop_ptr = flip_flop.Lock()) {
      return flip_flop_ptr->current_state;
    }
    return false;
  }
};

struct FlipFlopWidget : Object::WidgetBase {
  struct AnimationState {
    float light = 0;
  };
  AnimationState animation_state;
  std::unique_ptr<FlipFlopButton> button;

  FlipFlopWidget(ui::Widget* parent, WeakPtr<FlipFlop> weak_flip_flop)
      : WidgetBase(parent), button(new FlipFlopButton(this, weak_flip_flop)) {
    this->object = std::move(weak_flip_flop);
    auto rect = FlipFlopRect();
    button->local_to_parent = SkM44::Translate(rect.CenterX() - kYingYangButtonRadius,
                                               rect.CenterY() - kYingYangButtonRadius);
  }

  animation::Phase Tick(time::Timer& timer) override {
    auto current_state = LockObject<FlipFlop>()->current_state;
    button->WakeAnimationAt(wake_time);
    return animation::LinearApproach(current_state, timer.d, 10, animation_state.light);
  }
  void Draw(SkCanvas& canvas) const override {
    FlipFlopColor().draw(canvas);

    {  // Red indicator light
      SkPaint gradient;
      SkPoint center = {kFlipFlopWidth / 2, 2_cm};
      float radius = 0.5_mm;
      float a = animation_state.light;
      SkColor colors[] = {color::MixColors("#725016"_color, "#ff8786"_color, a),
                          color::MixColors("#2b1e07"_color, "#ff3e3e"_color, a)};
      gradient.setShader(SkGradientShader::MakeRadial(center + SkPoint(0, 0.25_mm), radius, colors,
                                                      0, 2, SkTileMode::kClamp));
      canvas.drawCircle(center, radius, gradient);

      SkPaint shine;
      SkColor shine_colors[] = {color::MixColors("#d2b788ff"_color, "#ffe8e8ff"_color, a),
                                color::MixColors("#d2b78800"_color, "#ffe8e800"_color, a),
                                color::MixColors("#d2b788ff"_color, "#ffe8e8ff"_color, a)};
      SkPoint shine_pts[] = {center + SkPoint{0, 0.5_mm}, center - SkPoint{0, 0.5_mm}};
      shine.setShader(
          SkGradientShader::MakeLinear(shine_pts, shine_colors, nullptr, 3, SkTileMode::kClamp));
      shine.setStyle(SkPaint::kStroke_Style);
      shine.setStrokeWidth(0.06_mm);
      shine.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 0.05_mm));
      canvas.drawCircle(center, radius - 0.1_mm, shine);

      SkPaint stroke;
      stroke.setColor(color::MixColors("#110902"_color, "#930d0d"_color, a));
      stroke.setStroke(SkPaint::kStroke_Style);
      stroke.setStrokeWidth(0.1_mm);
      canvas.drawCircle(center, radius + 0.04_mm, stroke);

      SkPaint red_glow;
      red_glow.setColor("#ff3e3e"_color);
      red_glow.setAlphaf(a);
      red_glow.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 0.5_mm));
      canvas.drawCircle(center, radius, red_glow);
    }

    DrawChildren(canvas);
  }
  SkPath Shape() const override { return SkPath::Rect(FlipFlopRect()); }
  bool CenteredAtZero() const override { return true; }

  void FillChildren(Vec<ui::Widget*>& children) override { children.push_back(button.get()); }
};

std::unique_ptr<ObjectWidget> FlipFlop::MakeWidget(ui::Widget* parent) {
  return std::make_unique<FlipFlopWidget>(parent, AcquireWeakPtr());
}
}  // namespace automat::library
