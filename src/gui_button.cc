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
#include "gui_constants.h"

namespace automaton::gui {

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

constexpr float kRadius = kMinimalTouchableSize / 2;
constexpr float kShadowSigma = kRadius / 10;
constexpr float kPressOffset = kRadius / 20;

} // namespace

float Button::Height() const {
  SkRect child_bounds = child->Shape().getBounds();
  return std::max(kMinimalTouchableSize, child_bounds.height() + 2 * kMargin);
}

SkRRect Button::RRect() const {
  vec2 p = Position();
  SkRect child_bounds = child->Shape().getBounds();
  float w = std::max(kMinimalTouchableSize, child_bounds.width() + 2 * kMargin);
  float h =
      std::max(kMinimalTouchableSize, child_bounds.height() + 2 * kMargin);
  return SkRRect::MakeRectXY(SkRect::MakeXYWH(p.X, p.Y, w, h), kRadius,
                             kRadius);
}

void Button::DrawButtonShadow(SkCanvas &canvas, SkColor bg) const {
  auto oval = RRect();
  oval.inset(kBorderWidth / 2, kBorderWidth / 2);
  oval.offset(0, -kPressOffset);
  SkPaint shadow_paint;
  shadow_paint.setColor(color::AdjustLightness(bg, -40));
  shadow_paint.setMaskFilter(
      SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, kShadowSigma, true));
  canvas.drawRRect(oval, shadow_paint);
}

void Button::DrawButtonFace(SkCanvas &canvas, animation::State &animation_state,
                            SkColor bg, SkColor fg) const {
  auto &press = press_ptr[animation_state];
  auto &hover = hover_ptr[animation_state];
  auto &filling = filling_ptr[animation_state];

  auto oval = RRect();
  oval.inset(kBorderWidth / 2, kBorderWidth / 2);
  float press_shift_y = press * -kPressOffset;
  auto pressed_oval = oval.makeOffset(0, press_shift_y);
  float lightness_adjust = hover * 10;

  SkPaint paint;
  SkPoint pts[2] = {{0, oval.rect().bottom()}, {0, oval.rect().top()}};
  SkColor colors[2] = {
      color::AdjustLightness(bg, lightness_adjust),       // top
      color::AdjustLightness(bg, lightness_adjust - 10)}; // bottom
  sk_sp<SkShader> gradient =
      SkGradientShader::MakeLinear(pts, colors, nullptr, 2, SkTileMode::kClamp);
  paint.setShader(gradient);
  canvas.drawRRect(pressed_oval, paint);

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
  canvas.drawRRect(pressed_oval, border);

  SkPaint fg_paint;
  fg_paint.setColor(fg);
  fg_paint.setAntiAlias(true);
  canvas.translate(pressed_oval.rect().centerX(),
                   pressed_oval.rect().centerY());
  child->DrawColored(canvas, animation_state, fg_paint);
  canvas.translate(-pressed_oval.rect().centerX(),
                   -pressed_oval.rect().centerY());
}

void Button::DrawButton(SkCanvas &canvas, animation::State &animation_state,
                        SkColor bg) const {
  DrawButtonShadow(canvas, bg);
  DrawButtonFace(canvas, animation_state, bg, 0xff000000);
}

// TODO: move the filling animation into a separate subclass
void Button::Draw(SkCanvas &canvas, animation::State &animation_state) const {
  auto &press = press_ptr[animation_state];
  auto &hover = hover_ptr[animation_state];
  auto &filling = filling_ptr[animation_state];
  filling.speed = 5;
  filling.target = Filled() ? 1 : 0;
  hover.Tick(animation_state);
  filling.Tick(animation_state);

  auto oval = RRect();
  oval.inset(kBorderWidth / 2, kBorderWidth / 2);
  constexpr SkColor color = 0xffd69d00;
  constexpr SkColor white = 0xffffffff;

  float lightness_adjust = hover * 10;
  float press_shift_y = press * -kPressOffset;
  auto pressed_oval = oval.makeOffset(0, press_shift_y);
  SkRRect pressed_outer_oval;
  pressed_oval.outset(kBorderWidth / 2 + kShadowSigma * 0,
                      kBorderWidth / 2 + kShadowSigma * 0, &pressed_outer_oval);

  if (filling >= 0.999) {
    DrawButtonShadow(canvas, color);
    DrawButtonFace(canvas, animation_state, color, white);
  } else if (filling <= 0.001) {
    DrawButtonShadow(canvas, white);
    DrawButtonFace(canvas, animation_state, white, color);
  } else {
    DrawButtonShadow(canvas, color::MixColors(white, color, filling));
    float baseline = pressed_outer_oval.rect().fTop * (1 - filling) +
                     pressed_outer_oval.rect().fBottom * filling;
    constexpr int n_points = 6;
    vec2 points[n_points];
    static time::point base = animation_state.timer.now;
    float timeS = (animation_state.timer.now - base).count();
    constexpr float waving_x = kRadius / n_points / 2;
    float waving_y = waving_x * filling * (1 - filling) * 8;
    for (int i = 0; i < n_points; ++i) {
      float frac = i / float(n_points - 1);
      float a = (frac * 3 + timeS * 1) * 2 * M_PI;
      points[i].X = frac * pressed_outer_oval.rect().fRight +
                    (1 - frac) * pressed_outer_oval.rect().fLeft +
                    cos(a) * waving_x;
      points[i].Y = baseline + sin(a) * waving_y;
    }
    points[0].X = pressed_outer_oval.rect().fLeft;
    points[n_points - 1].X = pressed_outer_oval.rect().fRight;
    constexpr float wave_width = 2 * waving_x;
    SkPath waves_clip;
    waves_clip.moveTo(points[0].X, points[0].Y);
    for (int i = 0; i < n_points - 1; ++i) {
      waves_clip.cubicTo(points[i].X + wave_width, points[i].Y,
                         points[i + 1].X - wave_width, points[i + 1].Y,
                         points[i + 1].X, points[i + 1].Y);
    }
    waves_clip.lineTo(pressed_outer_oval.rect().fRight,
                      pressed_outer_oval.rect().fTop);
    waves_clip.lineTo(pressed_outer_oval.rect().fLeft,
                      pressed_outer_oval.rect().fTop);

    canvas.save();
    canvas.clipPath(waves_clip, SkClipOp::kDifference, true);
    DrawButtonFace(canvas, animation_state, white, color);
    canvas.restore();
    canvas.save();
    canvas.clipPath(waves_clip, SkClipOp::kIntersect, true);
    DrawButtonFace(canvas, animation_state, color, white);
    canvas.restore();
  }
}

SkPath Button::Shape() const { return SkPath::RRect(RRect()); }

struct ButtonAction : public Action {
  Button &button;
  ButtonAction(Button &button) : button(button) {}

  void Begin(gui::Pointer &pointer) override {
    button.Activate();
    button.press_action_count++;
    for (auto &press : button.press_ptr) {
      press = 1;
    }
  }

  void Update(gui::Pointer &) override {}

  void End() override {
    button.press_action_count--;
    if (button.press_action_count == 0) {
      for (auto &press : button.press_ptr) {
        press = 0;
      }
    }
  }

  void Draw(SkCanvas &canvas, animation::State &animation_state) override {}
};

std::unique_ptr<Action> Button::ButtonDownAction(Pointer &pointer,
                                                 PointerButton pointer_button) {
  if (pointer_button == PointerButton::kMouseLeft) {
    return std::make_unique<ButtonAction>(*this);
  }
  return nullptr;
}

Button::Button(Widget *parent, std::unique_ptr<Widget> &&child)
    : ReparentableWidget(parent), child(std::move(child)) {
  ReparentableWidget::TryReparent(this->child.get(), this);
}

} // namespace automaton::gui
