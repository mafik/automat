#define _USE_MATH_DEFINES
#include "gui_button.h"

#include <cmath>

#include <include/core/SkBlurTypes.h>
#include <include/core/SkClipOp.h>
#include <include/core/SkColor.h>
#include <include/core/SkMaskFilter.h>
#include <include/core/SkRRect.h>
#include <include/effects/SkGradientShader.h>

#include "color.h"

namespace automaton::gui {

Widget *Button::ParentWidget() { return parent_widget; }

void Button::PointerOver(Pointer &pointer, animation::State &animation_state) {
  auto &hover = hover_ptr[animation_state];
  hover.target = 1;
  hover.speed = 5;
  pointer.PushIcon(Pointer::kIconHand);
}

void Button::PointerLeave(Pointer &pointer, animation::State &animation_state) {
  hover_ptr[animation_state].target = 0;
  pointer.PopIcon();
}

namespace {

constexpr float kHeight = 0.008f;
constexpr float kWidth = kHeight;
constexpr float kRadius = kHeight / 2;
constexpr float kShadowSigma = kRadius / 10;
constexpr float kBorderWidth = kRadius / 10;

const SkRect &ButtonOval() {
  static SkRect oval = SkRect::MakeLTRB(0, 0, kWidth, kHeight);
  return oval;
}

void DrawBlur(SkCanvas &canvas, SkColor color, float outset, float r, float y) {
  SkAutoCanvasRestore auto_canvas_restore(&canvas, true);
  SkPaint paint;
  paint.setColor(color);
  paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, r, true));
  auto oval = ButtonOval().makeOutset(outset, outset);
  canvas.translate(0, y);
  canvas.drawOval(oval, paint);
  canvas.translate(0, -y);
}

} // namespace

void Button::Draw(SkCanvas &canvas, animation::State &animation_state) const {
  auto &press = press_ptr[animation_state];
  auto &hover = hover_ptr[animation_state];
  auto &filling = filling_ptr[animation_state];
  filling.speed = 5;
  filling.target = filled ? 1 : 0;
  press.Tick(animation_state);
  hover.Tick(animation_state);
  filling.Tick(animation_state);

  auto oval = ButtonOval().makeInset(kBorderWidth / 2, kBorderWidth / 2);
  constexpr SkColor color = 0xffd69d00;
  constexpr SkColor white = 0xffffffff;

  float lightness_adjust = hover * 10;
  float press_shift_y = press * -kShadowSigma;
  auto pressed_oval = oval.makeOffset(0, press_shift_y);
  auto pressed_outer_oval = pressed_oval.makeOutset(
      kBorderWidth / 2 + kShadowSigma * 0, kBorderWidth / 2 + kShadowSigma * 0);

  auto DrawShadow = [&](SkColor color) {
    // Shadow
    SkPaint shadow_paint;
    shadow_paint.setColor(color::AdjustLightness(color, -40));
    shadow_paint.setMaskFilter(
        SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, kShadowSigma, true));
    canvas.drawOval(oval.makeOffset(0, -kShadowSigma), shadow_paint);
  };

  auto DrawBase = [&](SkColor bg, SkColor fg) {
    SkPaint paint;
    SkPoint pts[2] = {{0, kHeight}, {0, 0}};
    SkColor colors[2] = {
        color::AdjustLightness(bg, lightness_adjust),       // top
        color::AdjustLightness(bg, lightness_adjust - 10)}; // bottom
    sk_sp<SkShader> gradient = SkGradientShader::MakeLinear(
        pts, colors, nullptr, 2, SkTileMode::kClamp);
    paint.setShader(gradient);
    canvas.drawOval(pressed_oval, paint);

    SkPaint border;
    SkColor border_colors[2] = {
        color::AdjustLightness(bg, lightness_adjust + 10),
        color::AdjustLightness(bg, lightness_adjust - 20)};
    sk_sp<SkShader> border_gradient = SkGradientShader::MakeLinear(
        pts, border_colors, nullptr, 2, SkTileMode::kClamp);
    border.setShader(border_gradient);
    border.setStyle(SkPaint::kStroke_Style);
    border.setAntiAlias(true);
    border.setStrokeWidth(kBorderWidth);
    canvas.drawOval(pressed_oval, border);

    SkPaint fg_paint;
    fg_paint.setColor(fg);
    fg_paint.setAntiAlias(true);
    canvas.translate(kWidth / 2, kHeight / 2 + press_shift_y);
    child->DrawColored(canvas, animation_state, fg_paint);
    canvas.translate(-kWidth / 2, -kHeight / 2 - press_shift_y);
  };

  if (filling >= 0.999) {
    DrawShadow(color);
    DrawBase(color, white);
  } else if (filling <= 0.001) {
    DrawShadow(white);
    DrawBase(white, color);
  } else {
    DrawShadow(color::MixColors(white, color, filling));
    float baseline = pressed_outer_oval.fTop * (1 - filling) +
                     pressed_outer_oval.fBottom * filling;
    constexpr int n_points = 6;
    vec2 points[n_points];
    static time::point base = animation_state.timer.now;
    float timeS = (animation_state.timer.now - base).count();
    constexpr float waving_x = kRadius / n_points / 2;
    float waving_y = waving_x * filling * (1 - filling) * 8;
    for (int i = 0; i < n_points; ++i) {
      float frac = i / float(n_points - 1);
      float a = (frac * 3 + timeS * 1) * 2 * M_PI;
      points[i].X = frac * pressed_outer_oval.fRight +
                    (1 - frac) * pressed_outer_oval.fLeft + cos(a) * waving_x;
      points[i].Y = baseline + sin(a) * waving_y;
    }
    points[0].X = pressed_outer_oval.fLeft;
    points[n_points - 1].X = pressed_outer_oval.fRight;
    constexpr float wave_width = 2 * waving_x;
    SkPath waves_clip;
    waves_clip.moveTo(points[0].X, points[0].Y);
    for (int i = 0; i < n_points - 1; ++i) {
      waves_clip.cubicTo(points[i].X + wave_width, points[i].Y,
                         points[i + 1].X - wave_width, points[i + 1].Y,
                         points[i + 1].X, points[i + 1].Y);
    }
    waves_clip.lineTo(pressed_outer_oval.fRight, pressed_outer_oval.fTop);
    waves_clip.lineTo(pressed_outer_oval.fLeft, pressed_outer_oval.fTop);

    canvas.save();
    canvas.clipPath(waves_clip, SkClipOp::kDifference, true);
    DrawBase(white, color);
    canvas.restore();
    canvas.save();
    canvas.clipPath(waves_clip, SkClipOp::kIntersect, true);
    DrawBase(color, white);
    canvas.restore();
  }
}

SkPath Button::Shape() const {
  static SkPath path = SkPath::Oval(ButtonOval());
  return path;
}

struct ButtonAction : public Action {
  Button &button;
  ButtonAction(Button &button) : button(button) {}

  void Begin(gui::Pointer &pointer) override {
    button.Activate();
    button.press_action_count++;
    for (auto &shadow : button.press_ptr) {
      shadow.target = 1;
      shadow.speed = 60;
    }
  }

  void Update(gui::Pointer &) override {}

  void End() override {
    button.press_action_count--;
    if (button.press_action_count == 0) {
      for (auto &shadow : button.press_ptr) {
        shadow.target = 0;
        shadow.speed = 5;
      }
    }
  }

  void Draw(SkCanvas &canvas, animation::State &animation_state) override {}
};

std::unique_ptr<Action> Button::ButtonDownAction(Pointer &pointer,
                                                 PointerButton pointer_button,
                                                 vec2 contact_point) {
  if (pointer_button == PointerButton::kMouseLeft) {
    return std::make_unique<ButtonAction>(*this);
  }
  return nullptr;
}

Button::Button(Widget *parent_widget, std::unique_ptr<Widget> &&child)
    : parent_widget(parent_widget), child(std::move(child)) {}

void Button::ToggleFill() { filled = !filled; }

} // namespace automaton::gui