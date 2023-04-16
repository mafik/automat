#include "gui_button.h"

#include <include/core/SkBlurTypes.h>
#include <include/core/SkColor.h>
#include <include/core/SkMaskFilter.h>
#include <include/core/SkRRect.h>
#include <include/effects/SkGradientShader.h>

#include "color.h"

namespace automaton::gui {

Widget *Button::ParentWidget() { return parent_widget; }

void Button::PointerOver(Pointer &pointer, animation::State &animation_state) {
  auto &inset = inset_shadow_inset[animation_state];
  inset.target = 1;
  inset.speed = 5;
  pointer.PushIcon(Pointer::kIconHand);
}

void Button::PointerLeave(Pointer &pointer, animation::State &animation_state) {
  inset_shadow_inset[animation_state].target = 0;
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
  auto &press = inset_shadow_press[animation_state];
  auto &hover = inset_shadow_inset[animation_state];
  press.Tick(animation_state);
  hover.Tick(animation_state);

  auto oval = ButtonOval().makeInset(kBorderWidth / 2, kBorderWidth / 2);
  constexpr SkColor color = 0xffd69d00;
  constexpr SkColor white = 0xffffffff;

  float bg_adjust = hover * 10;

  auto DrawBase = [&](SkColor bg, SkColor fg) {
    // Shadow
    SkPaint shadow_paint;
    shadow_paint.setColor(
        color::SetAlpha(color::AdjustLightness(bg, -40), 1.0f));
    shadow_paint.setMaskFilter(
        SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, kShadowSigma, true));
    canvas.drawOval(oval.makeOffset(0, -kShadowSigma), shadow_paint);

    SkPaint paint;
    SkPoint pts[2] = {{0, kHeight}, {0, 0}};
    SkColor colors[2] = {color::AdjustLightness(bg, bg_adjust),       // top
                         color::AdjustLightness(bg, bg_adjust - 10)}; // bottom
    sk_sp<SkShader> gradient = SkGradientShader::MakeLinear(
        pts, colors, nullptr, 2, SkTileMode::kClamp);
    paint.setShader(gradient);
    canvas.drawOval(oval, paint);

    SkPaint border;
    SkColor border_colors[2] = {color::AdjustLightness(bg, bg_adjust + 10),
                                color::AdjustLightness(bg, bg_adjust - 20)};
    sk_sp<SkShader> border_gradient = SkGradientShader::MakeLinear(
        pts, border_colors, nullptr, 2, SkTileMode::kClamp);
    border.setShader(border_gradient);
    border.setStyle(SkPaint::kStroke_Style);
    border.setAntiAlias(true);
    border.setStrokeWidth(kBorderWidth);
    canvas.drawOval(oval, border);

    SkPaint fg_paint;
    fg_paint.setColor(fg);
    fg_paint.setAntiAlias(true);
    canvas.translate(kWidth / 2, kHeight / 2);
    child->DrawColored(canvas, animation_state, fg_paint);
    canvas.translate(-kWidth / 2, -kHeight / 2);
  };

  DrawBase(white, color);
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
    for (auto &shadow : button.inset_shadow_press) {
      shadow.target = 1;
      shadow.speed = 15;
    }
  }

  void Update(gui::Pointer &) override {}

  void End() override {
    button.press_action_count--;
    if (button.press_action_count == 0) {
      for (auto &shadow : button.inset_shadow_press) {
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

} // namespace automaton::gui
