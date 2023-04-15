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
constexpr float kShadowSigma = kRadius / 8;

SkPaint GetBackgroundPaint() {
  SkPaint paint;
  SkPoint pts[3] = {{0, 0}, {0, 0.8f * kHeight}, {0, kHeight}};
  SkColor base_color = 0xfff7f1ed;
  base_color = 0xffebac3f;
  SkScalar hsv[3];
  SkColorToHSV(base_color, hsv);

  SkScalar bottom_hsv[3];
  memcpy(bottom_hsv, hsv, sizeof(hsv));
  bottom_hsv[2] *= 0.8f;
  SkColor bottom_color = SkHSVToColor(bottom_hsv);

  SkScalar top_hsv[3];
  memcpy(top_hsv, hsv, sizeof(hsv));
  top_hsv[2] *= 0.95f;
  SkColor top_color = SkHSVToColor(top_hsv);

  SkColor colors[3] = {bottom_color, base_color, top_color};
  sk_sp<SkShader> gradient =
      SkGradientShader::MakeLinear(pts, colors, nullptr, 3, SkTileMode::kClamp);
  paint.setShader(gradient);
  return paint;
}

SkPaint GetBorderPaint() {
  SkPaint paint;
  paint.setColor(color::FromHex(0x8f9092));
  paint.setStyle(SkPaint::kStroke_Style);
  paint.setAntiAlias(true);
  paint.setStrokeWidth(0.00025f);
  return paint;
}

SkPaint &GetShadowPaint() {
  static SkPaint paint = []() -> SkPaint {
    SkPaint paint;
    // paint.setColor(0x40000000);
    paint.setMaskFilter(
        SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, kShadowSigma, true));
    return paint;
  }();
  return paint;
}

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

  float depth = std::max(press.value, hover.value / 2);

  auto oval = ButtonOval();

  // Draw highlight above
  DrawBlur(canvas, 0x80FFFFFF, 0, kShadowSigma, kShadowSigma);

  // Draw shadow above
  DrawBlur(canvas, 0x40000000, 0, kShadowSigma / 2, kShadowSigma / 2);

  // Draw shadow below
  DrawBlur(canvas, 0x20000000, 0, kShadowSigma * 2, -kShadowSigma * 2);

  // Draw highlight below
  DrawBlur(canvas, 0xA0FFFFFF, 0.00015f, 0.00020, -0.00020);

  canvas.drawOval(oval, GetBackgroundPaint());
  {
    SkAutoCanvasRestore auto_canvas_restore(&canvas, true);
    canvas.clipRRect(SkRRect::MakeOval(oval));
    auto shadow_oval =
        oval.makeInset(kShadowSigma * 2 * depth, kShadowSigma * 2 * depth);
    auto shape = SkPath::Oval(shadow_oval);
    shape.toggleInverseFillType();
    SkPaint paint = GetShadowPaint();
    paint.setMaskFilter(SkMaskFilter::MakeBlur(
        kNormal_SkBlurStyle, kShadowSigma * (1 + depth), true));
    paint.setAlpha(0x60 + depth * 0x40);
    canvas.drawPath(shape, paint);
    shape.toggleInverseFillType();
  }
  canvas.drawOval(oval, GetBorderPaint());

  canvas.translate(kWidth / 2, kHeight / 2);
  child->Draw(canvas, animation_state);
  canvas.translate(-kWidth / 2, -kHeight / 2);
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
