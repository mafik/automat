#include "gui_button.h"

#include <include/core/SkBlurTypes.h>
#include <include/core/SkMaskFilter.h>
#include <include/effects/SkGradientShader.h>

#include "color.h"

namespace automaton::gui {

Widget *Button::ParentWidget() { return parent_widget; }

void Button::PointerOver(Pointer &pointer, animation::State &) {
  pointer.PushIcon(Pointer::kIconHand);
}

void Button::PointerLeave(Pointer &pointer, animation::State &) {
  pointer.PopIcon();
}

namespace {

constexpr float kHeight = 0.008f;
constexpr float kRadius = kHeight / 2;

SkPaint GetBackgroundPaint() {
  SkPaint paint;
  SkPoint pts[3] = {{0, 0}, {0, 0.8f * kHeight}, {0, kHeight}};
  SkColor colors[3] = {color::FromHex(0xd8d9db), color::FromHex(0xffffff),
                       color::FromHex(0xfdfdfd)};
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
  return paint;
}

SkPaint &GetShadowPaint() {
  static SkPaint paint = []() -> SkPaint {
    SkPaint paint;
    paint.setColor(color::FromHex(0xCECFD1));
    paint.setMaskFilter(
        SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, kRadius / 2, true));
    return paint;
  }();
  return paint;
}

} // namespace

void Button::Draw(SkCanvas &canvas, animation::State &animation_state) const {
  auto shape = Shape();
  canvas.drawPath(shape, GetBackgroundPaint());
  // canvas.save();
  {
    SkAutoCanvasRestore autoCanvasRestore(&canvas, true);
    canvas.clipPath(shape);
    shape.toggleInverseFillType();
    canvas.drawPath(shape, GetShadowPaint());
    shape.toggleInverseFillType();
  }
  // canvas.restore();
  canvas.drawPath(shape, GetBorderPaint());
}

SkPath Button::Shape() const {
  static SkPath path = SkPath::Circle(0, 0, kRadius);
  return path;
}

std::unique_ptr<Action> Button::ButtonDownAction(Pointer &, PointerButton,
                                                 vec2 contact_point) {
  return nullptr;
}

} // namespace automaton::gui