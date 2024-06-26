#include "gui_button.hh"

#include <include/core/SkBlurTypes.h>
#include <include/core/SkClipOp.h>
#include <include/core/SkColor.h>
#include <include/core/SkMaskFilter.h>
#include <include/core/SkRRect.h>
#include <include/effects/SkGradientShader.h>

#include <cmath>

#include "color.hh"
#include "gui_constants.hh"

namespace automat::gui {

void Button::PointerOver(Pointer& pointer, animation::Display& display) {
  auto& hover = hover_ptr[display];
  hover.target = 1;
  hover.speed = 5;
  pointer.PushIcon(Pointer::kIconHand);
}

void Button::PointerLeave(Pointer& pointer, animation::Display& display) {
  hover_ptr[display].target = 0;
  pointer.PopIcon();
}

namespace {

constexpr float kRadius = kMinimalTouchableSize / 2;
constexpr float kShadowSigma = kRadius / 10;

}  // namespace

float Button::Height() const {
  SkRect child_bounds = ChildBounds();
  return std::max(kMinimalTouchableSize, child_bounds.height() + 2 * kMargin);
}

SkRRect Button::RRect() const {
  SkRect child_bounds = ChildBounds();
  float w = std::max(kMinimalTouchableSize, child_bounds.width() + 2 * kMargin);
  float h = std::max(kMinimalTouchableSize, child_bounds.height() + 2 * kMargin);
  return SkRRect::MakeRectXY(SkRect::MakeXYWH(0, 0, w, h), kRadius, kRadius);
}

void Button::DrawButtonShadow(SkCanvas& canvas, SkColor bg) const {
  auto oval = RRect();
  oval.inset(kBorderWidth / 2, kBorderWidth / 2);
  oval.offset(0, -kPressOffset);
  SkPaint shadow_paint;
  shadow_paint.setColor(color::AdjustLightness(bg, -40));
  shadow_paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, kShadowSigma, true));
  canvas.drawRRect(oval, shadow_paint);
}

void Button::DrawButtonFace(DrawContext& ctx, SkColor bg, SkColor fg, Widget* child) const {
  auto& canvas = ctx.canvas;
  auto& display = ctx.display;
  auto& hover = hover_ptr[display];
  SkRect child_bounds = child->Shape().getBounds();

  auto oval = RRect();
  oval.inset(kBorderWidth / 2, kBorderWidth / 2);
  float press_shift_y = PressRatio() * -kPressOffset;
  auto pressed_oval = oval.makeOffset(0, press_shift_y);
  float lightness_adjust = hover * 10;

  SkPaint paint;
  SkPoint pts[2] = {{0, oval.rect().bottom()}, {0, oval.rect().top()}};
  SkColor colors[2] = {color::AdjustLightness(bg, lightness_adjust),        // top
                       color::AdjustLightness(bg, lightness_adjust - 10)};  // bottom
  sk_sp<SkShader> gradient =
      SkGradientShader::MakeLinear(pts, colors, nullptr, 2, SkTileMode::kClamp);
  paint.setShader(gradient);
  canvas.drawRRect(pressed_oval, paint);

  SkPaint border;
  SkColor border_colors[2] = {color::AdjustLightness(bg, lightness_adjust + 10),
                              color::AdjustLightness(bg, lightness_adjust - 20)};
  sk_sp<SkShader> border_gradient =
      SkGradientShader::MakeLinear(pts, border_colors, nullptr, 2, SkTileMode::kClamp);
  border.setShader(border_gradient);
  border.setStyle(SkPaint::kStroke_Style);
  border.setAntiAlias(true);
  border.setStrokeWidth(kBorderWidth);
  canvas.drawRRect(pressed_oval, border);

  if (auto paint = PaintMixin::Get(child)) {
    paint->setColor(fg);
    paint->setAntiAlias(true);
  }
  if (child) {
    canvas.save();
    canvas.translate(
        pressed_oval.rect().centerX() - child_bounds.width() / 2 - child_bounds.left(),
        pressed_oval.rect().centerY() - child_bounds.height() / 2 + child_bounds.bottom());
    child->Draw(ctx);
    canvas.restore();
  }
}

void Button::DrawButton(DrawContext& ctx, SkColor bg) const {
  DrawButtonShadow(ctx.canvas, bg);
  auto child = Child();
  DrawButtonFace(ctx, bg, 0xff000000, child);
}

void Button::Draw(DrawContext& ctx) const {
  auto& display = ctx.display;
  auto& hover = hover_ptr[display];
  hover.Tick(display);

  auto bg = BackgroundColor();
  auto fg = ForegroundColor(ctx);
  DrawButtonShadow(ctx.canvas, bg);
  auto child = Child();
  DrawButtonFace(ctx, bg, fg, child);
}

void ToggleButton::Draw(DrawContext& ctx) const {
  auto& canvas = ctx.canvas;
  auto& display = ctx.display;
  auto& hover = hover_ptr[display];
  auto& filling = filling_ptr[display];
  filling.speed = 5;
  filling.target = Filled() ? 1 : 0;
  hover.Tick(display);
  filling.Tick(display);

  auto oval = RRect();
  oval.inset(kBorderWidth / 2, kBorderWidth / 2);
  SkColor bg = BackgroundColor();
  SkColor fg = ForegroundColor(ctx);

  float lightness_adjust = hover * 10;
  float press_shift_y = PressRatio() * -kPressOffset;
  auto pressed_oval = oval.makeOffset(0, press_shift_y);
  SkRRect pressed_outer_oval;
  pressed_oval.outset(kBorderWidth / 2 + kShadowSigma * 0, kBorderWidth / 2 + kShadowSigma * 0,
                      &pressed_outer_oval);

  if (filling >= 0.999) {
    DrawButtonShadow(canvas, fg);
    auto child = FilledChild();
    DrawButtonFace(ctx, fg, bg, child);
  } else if (filling <= 0.001) {
    DrawButtonShadow(canvas, bg);
    auto child = Child();
    DrawButtonFace(ctx, bg, fg, child);
  } else {
    DrawButtonShadow(canvas, color::MixColors(bg, fg, filling));
    float baseline = pressed_outer_oval.rect().fTop * (1 - filling) +
                     pressed_outer_oval.rect().fBottom * filling;
    constexpr int n_points = 6;
    Vec2 points[n_points];
    static time::SystemPoint base = display.timer.now;
    float timeS = (display.timer.now - base).count();
    constexpr float waving_x = kRadius / n_points / 2;
    float waving_y = waving_x * filling * (1 - filling) * 8;
    for (int i = 0; i < n_points; ++i) {
      float frac = i / float(n_points - 1);
      float a = (frac * 3 + timeS * 1) * 2 * M_PI;
      points[i].x = frac * pressed_outer_oval.rect().fRight +
                    (1 - frac) * pressed_outer_oval.rect().fLeft + cos(a) * waving_x;
      points[i].y = baseline + sin(a) * waving_y;
    }
    points[0].x = pressed_outer_oval.rect().fLeft;
    points[n_points - 1].x = pressed_outer_oval.rect().fRight;
    constexpr float wave_width = 2 * waving_x;
    SkPath waves_clip;
    waves_clip.moveTo(points[0].x, points[0].y);
    for (int i = 0; i < n_points - 1; ++i) {
      waves_clip.cubicTo(points[i].x + wave_width, points[i].y, points[i + 1].x - wave_width,
                         points[i + 1].y, points[i + 1].x, points[i + 1].y);
    }
    waves_clip.lineTo(pressed_outer_oval.rect().fRight, pressed_outer_oval.rect().fTop);
    waves_clip.lineTo(pressed_outer_oval.rect().fLeft, pressed_outer_oval.rect().fTop);

    canvas.save();
    canvas.clipPath(waves_clip, SkClipOp::kDifference, true);
    auto child = Child();
    auto filled_child = FilledChild();
    DrawButtonFace(ctx, bg, fg, child);
    canvas.restore();
    canvas.save();
    canvas.clipPath(waves_clip, SkClipOp::kIntersect, true);
    DrawButtonFace(ctx, fg, bg, filled_child);
    canvas.restore();
  }
}

SkPath Button::Shape() const { return SkPath::RRect(RRect()); }

struct ButtonAction : public Action {
  Button& button;
  ButtonAction(Button& button) : button(button) {}

  void Begin(gui::Pointer& pointer) override {
    button.Activate(pointer);
    button.press_action_count++;
  }

  void Update(gui::Pointer&) override {}

  void End() override { button.press_action_count--; }

  void DrawAction(DrawContext&) override {}
};

std::unique_ptr<Action> Button::ButtonDownAction(Pointer& pointer, PointerButton pointer_button) {
  if (pointer_button == PointerButton::kMouseLeft) {
    return std::make_unique<ButtonAction>(*this);
  }
  return nullptr;
}

Button::Button() {}

SkRect Button::ChildBounds() const {
  if (auto child = Child()) return child->Shape().getBounds();
  return SkRect::MakeEmpty();
}

}  // namespace automat::gui
