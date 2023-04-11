#include "gui_button.h"

#include <include/core/SkBlurTypes.h>
#include <include/core/SkMaskFilter.h>
#include <include/core/SkRRect.h>
#include <include/effects/SkGradientShader.h>

#include "color.h"

namespace automaton::gui {

Widget *Button::ParentWidget() { return parent_widget; }

void Button::PointerOver(Pointer &pointer, animation::State &animation_state) {
  inset_shadow_inset[animation_state].target = 1;
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

SkPaint GetBackgroundPaint() {
  SkPaint paint;
  SkPoint pts[3] = {{0, 0}, {0, 0.8f * kHeight}, {0, kHeight}};
  SkColor colors[3] = {0xffd8d9db, 0xffffffff, 0xfffdfdfd};
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
    paint.setColor(0x40000000);
    paint.setMaskFilter(
        SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, kShadowSigma, true));
    return paint;
  }();
  return paint;
}

const SkRect &ButtonOval() {
  static SkRect oval = SkRect::MakeWH(kHeight, kHeight);
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
}

} // namespace

void Button::Draw(SkCanvas &canvas, animation::State &animation_state) const {
  auto &sigma = inset_shadow_sigma[animation_state];
  auto &inset = inset_shadow_inset[animation_state];
  sigma.Tick(animation_state);
  inset.Tick(animation_state);

  double time = animation_state.timer.now.time_since_epoch().count();
  picture->seekFrameTime(fmod(time, picture->duration()), nullptr);

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
  // canvas.save();
  {
    SkAutoCanvasRestore auto_canvas_restore(&canvas, true);
    canvas.clipRRect(SkRRect::MakeOval(oval));
    auto shadow_oval =
        oval.makeInset(kShadowSigma * inset, kShadowSigma * inset);
    auto shape = SkPath::Oval(shadow_oval);
    shape.toggleInverseFillType();
    canvas.drawPath(shape, GetShadowPaint());
    shape.toggleInverseFillType();
  }
  // canvas.restore();
  canvas.drawOval(oval, GetBorderPaint());

  {
    SkAutoCanvasRestore auto_canvas_restore(&canvas, true);
    SkSize size = picture->size();
    float scaleX = oval.width() / size.width();
    float scaleY = oval.height() / size.height();
    float scale = std::min(scaleX, scaleY);
    size.fHeight *= scale;
    size.fWidth *= scale;
    canvas.translate((kWidth - size.width()) / 2,
                     kHeight - (kHeight - size.height()) / 2);
    canvas.scale(scale, -scale);
    // std::swap(oval.fTop, oval.fBottom);
    picture->render(&canvas);
  }
}

SkPath Button::Shape() const {
  static SkPath path = SkPath::Oval(ButtonOval());
  return path;
}

std::unique_ptr<Action> Button::ButtonDownAction(Pointer &, PointerButton,
                                                 vec2 contact_point) {
  return nullptr;
}

Button::Button(Widget *parent_widget, sk_sp<skottie::Animation> picture)
    : parent_widget(parent_widget), picture(picture) {}

} // namespace automaton::gui
