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
#include "arcline.hh"
#include "argument.hh"
#include "color.hh"
#include "library_macros.hh"
#include "sincos.hh"
#include "textures.hh"
#include "time.hh"

namespace automat::library {

DEFINE_PROTO(FlipFlop);

constexpr float kYingYangRadius = 1.2_cm / 2 - 1_mm;
constexpr float kYingYangRadiusSmall = kYingYangRadius / 2;
constexpr float kYingYangButtonRadius = kYingYangRadius + 0.5_mm;
constexpr float kFlipFlopWidth = 1.8_cm;

struct FlipFlopIcon : PaintDrawable {
  SkRect onGetBounds() override { return Rect::MakeCircleR(1_mm); }
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

static sk_sp<SkImage>& FlipFlopColor() {
  static auto image =
      MakeImageFromAsset(embedded::assets_flip_flop_color_webp)->withDefaultMipmaps();
  return image;
}

bool FlipFlopButton::Filled() const { return (flip_flop && flip_flop->current_state); }

FlipFlop::FlipFlop() { button.flip_flop = this; }
string_view FlipFlop::Name() const { return "Flip-Flop"; }
std::unique_ptr<Object> FlipFlop::Clone() const {
  auto ret = std::make_unique<FlipFlop>();
  ret->current_state = current_state;
  return ret;
}
void FlipFlop::Draw(gui::DrawContext& dctx) const {
  auto& canvas = dctx.canvas;
  auto img = FlipFlopColor();
  float s = kFlipFlopWidth / img->width();
  float height = s * img->height();
  auto m = canvas.getLocalToDevice();
  canvas.scale(s, -s);
  canvas.drawImage(img.get(), 0, -img->height(), kDefaultSamplingOptions);
  canvas.setMatrix(m);

  {  // Red indicator light
    auto animation_state = animation_states.Find(dctx.display);
    if (animation_state == nullptr) {
      animation_state = &animation_states[dctx.display];
      animation_state->light.value = current_state;
    }
    animation_state->light.target = current_state;
    animation_state->light.Tick(dctx.display);
    SkPaint gradient;
    SkPoint center = {kFlipFlopWidth / 2, 2_cm};
    float radius = 0.5_mm;
    float a = animation_state->light.value;
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

  DrawChildren(dctx);
}
Rect FlipFlopRect() {
  auto img = FlipFlopColor();
  float s = kFlipFlopWidth / img->width();
  return Rect::MakeZeroWH(kFlipFlopWidth, s * img->height());
}
SkPath FlipFlop::Shape(animation::Display*) const { return SkPath::Rect(FlipFlopRect()); }

ControlFlow FlipFlop::VisitChildren(gui::Visitor& visitor) {
  Widget* children[] = {&button};
  return visitor(children);
}

SkMatrix FlipFlop::TransformToChild(const Widget& child, animation::Display*) const {
  auto rect = FlipFlopRect();
  if (&child == &button) {
    return SkMatrix::Translate(-rect.CenterX() + kYingYangButtonRadius,
                               -rect.CenterY() + kYingYangButtonRadius);
  }
  return SkMatrix::I();
}

LongRunning* FlipFlop::OnRun(Location& here) {
  current_state = !current_state;

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

  return nullptr;
}

void YingYangIcon::Draw(gui::DrawContext& dctx) const {
  auto& canvas = dctx.canvas;
  Rect rect = Rect::MakeCircleR(kYingYangRadius);
  ArcLine tear = ArcLine(Vec2(0, kYingYangRadius), 0_deg);
  tear.TurnConvex(180_deg, -kYingYangRadius);
  tear.TurnConvex(180_deg, -kYingYangRadiusSmall);
  tear.TurnConvex(180_deg, kYingYangRadiusSmall);
  auto black_path = tear.ToPath();
  black_path.addCircle(0, kYingYangRadiusSmall, kYingYangRadiusSmall / 4);
  black_path.addCircle(0, -kYingYangRadiusSmall, kYingYangRadiusSmall / 4);
  canvas.drawPath(black_path, paint);
}
SkPath YingYangIcon::Shape(animation::Display*) const {
  return SkPath::Circle(0, 0, kYingYangRadius);
}
SkRRect FlipFlopButton::RRect() const {
  SkRect oval = SkRect::MakeXYWH(0, 0, 2 * kYingYangButtonRadius, 2 * kYingYangButtonRadius);
  return SkRRect::MakeOval(oval);
}

void FlipFlopButton::Activate(gui::Pointer&) { flip_flop->here->ScheduleRun(); }

SkColor FlipFlopButton::ForegroundColor(gui::DrawContext&) const { return "#1d1d1d"_color; }
SkColor FlipFlopButton::BackgroundColor() const { return "#eae9e8"_color; }
void FlipFlopButton::TweakShadow(float& sigma, float& offset) const {
  sigma = kYingYangButtonRadius / 5;
  offset = -kYingYangRadiusSmall / 2;
}

void FlipFlop::Args(std::function<void(Argument&)> cb) { cb(flip_arg); }
}  // namespace automat::library