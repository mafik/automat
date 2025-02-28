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

#include <memory>

#include "../build/generated/embedded.hh"
#include "animation.hh"
#include "arcline.hh"
#include "argument.hh"
#include "color.hh"
#include "gui_button.hh"
#include "sincos.hh"
#include "textures.hh"

using namespace maf;
using namespace std;

namespace automat::library {

constexpr float kYingYangRadius = 1.2_cm / 2 - 1_mm;
constexpr float kYingYangRadiusSmall = kYingYangRadius / 2;
constexpr float kYingYangButtonRadius = kYingYangRadius + 0.5_mm;
constexpr float kFlipFlopWidth = 1.8_cm;

struct FlipFlopIcon : PaintDrawable {
  void onDraw(SkCanvas* canvas) override { canvas->drawCircle(0, 0, 1_mm, paint); }
};

struct FlipFlopTarget : Argument {
  using Argument::Argument;
  FlipFlopIcon icon;

  PaintDrawable& Icon() override { return icon; }
  bool IsOn(Location& here) const override {
    if (auto flip_flop = here.As<FlipFlop>()) {
      return flip_flop->current_state;
    }
    return false;
  }
};

FlipFlopTarget flip_arg("flip", Argument::kOptional);

bool FlipFlopButton::Filled() const { return (flip_flop && flip_flop->current_state); }

// SkRRect FlipFlopButton::RRect() const {
//   SkRect oval = SkRect::MakeXYWH(0, 0, 2 * kYingYangButtonRadius, 2 * kYingYangButtonRadius);
//   return SkRRect::MakeOval(oval);
// }
// SkColor FlipFlopButton::ForegroundColor() const { return "#1d1d1d"_color; }
// SkColor FlipFlopButton::BackgroundColor() const { return "#eae9e8"_color; }
FlipFlopButton::FlipFlopButton()
    : gui::ToggleButton(
          make_shared<gui::ColoredButton>(
              make_shared<YingYangIcon>(),
              gui::ColoredButtonArgs{.fg = "#eae9e8"_color,
                                     .bg = "#1d1d1d"_color,
                                     .radius = kYingYangButtonRadius,
                                     .on_click =
                                         [this](gui::Pointer&) {
                                           if (auto h = this->flip_flop->here.lock()) {
                                             h->ScheduleRun();
                                           }
                                         }}),
          make_shared<gui::ColoredButton>(
              make_shared<YingYangIcon>(),
              gui::ColoredButtonArgs{.fg = "#1d1d1d"_color,
                                     .bg = "#eae9e8"_color,
                                     .radius = kYingYangButtonRadius,
                                     .on_click = [this](gui::Pointer&) {
                                       if (auto h = this->flip_flop->here.lock()) {
                                         h->ScheduleRun();
                                       }
                                     }})) {}

static PersistentImage& FlipFlopColor() {
  static auto flip_flop_color = PersistentImage::MakeFromAsset(
      embedded::assets_flip_flop_color_webp, {.width = kFlipFlopWidth});
  return flip_flop_color;
}

Rect FlipFlopRect() {
  return Rect::MakeCornerZero(FlipFlopColor().width(), FlipFlopColor().height());
}
SkPath FlipFlop::Shape() const { return SkPath::Rect(FlipFlopRect()); }

FlipFlop::FlipFlop() : button(make_shared<FlipFlopButton>()) {
  button->flip_flop = this;
  auto rect = FlipFlopRect();
  button->local_to_parent = SkM44::Translate(rect.CenterX() - kYingYangButtonRadius,
                                             rect.CenterY() - kYingYangButtonRadius);
}
string_view FlipFlop::Name() const { return "Flip-Flop"; }
std::shared_ptr<Object> FlipFlop::Clone() const {
  auto ret = std::make_shared<FlipFlop>();
  ret->current_state = current_state;
  return ret;
}

animation::Phase FlipFlop::Tick(time::Timer& timer) {
  return animation::LinearApproach(current_state, timer.d, 10, animation_state.light);
}

void FlipFlop::Draw(SkCanvas& canvas) const {
  FlipFlopColor().draw(canvas);

  {  // Red indicator light
    SkPaint gradient;
    SkPoint center = {kFlipFlopWidth / 2, 2_cm};
    float radius = 0.5_mm;
    float a = animation_state.light;
    SkColor colors[] = {color::MixColors("#725016"_color, "#ff8786"_color, a),
                        color::MixColors("#2b1e07"_color, "#ff3e3e"_color, a)};
    gradient.setShader(SkGradientShader::MakeRadial(center + SkPoint(0, 0.25_mm), radius, colors, 0,
                                                    2, SkTileMode::kClamp));
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

void FlipFlop::FillChildren(maf::Vec<std::shared_ptr<Widget>>& children) {
  children.push_back(button);
}

void FlipFlop::OnRun(Location& here) {
  current_state = !current_state;
  WakeAnimation();
  button->WakeAnimation();
  flip_arg.InvalidateConnectionWidgets(here);

  if (current_state) {
    flip_arg.LoopLocations<bool>(here, [](Location& other) {
      other.ScheduleRun();
      return false;
    });
  } else {
    flip_arg.LoopLocations<bool>(here, [](Location& other) {
      if (other.long_running) {
        other.long_running->Cancel();
        other.long_running = nullptr;
      }
      return false;
    });
  }
}

void YingYangIcon::Draw(SkCanvas& canvas) const {
  ArcLine tear = ArcLine(Vec2(0, kYingYangRadius), 0_deg);
  tear.TurnConvex(180_deg, -kYingYangRadius);
  tear.TurnConvex(180_deg, -kYingYangRadiusSmall);
  tear.TurnConvex(180_deg, kYingYangRadiusSmall);
  auto black_path = tear.ToPath();
  black_path.addCircle(0, kYingYangRadiusSmall, kYingYangRadiusSmall / 4);
  black_path.addCircle(0, -kYingYangRadiusSmall, kYingYangRadiusSmall / 4);
  canvas.drawPath(black_path, paint);
}
SkPath YingYangIcon::Shape() const { return SkPath::Circle(0, 0, kYingYangRadius); }

void FlipFlop::Args(std::function<void(Argument&)> cb) { cb(flip_arg); }

void FlipFlop::SerializeState(Serializer& writer, const char* key) const {
  writer.Key(key);
  writer.Bool(current_state);
}

void FlipFlop::DeserializeState(Location& l, Deserializer& d) {
  Status status;
  d.Get(current_state, status);
  if (!OK(status)) {
    l.ReportError(status.ToStr());
  }
}
}  // namespace automat::library