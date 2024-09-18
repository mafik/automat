#include "gui_button.hh"

#include <include/core/SkBlurTypes.h>
#include <include/core/SkClipOp.h>
#include <include/core/SkColor.h>
#include <include/core/SkMaskFilter.h>
#include <include/core/SkRRect.h>
#include <include/effects/SkGradientShader.h>

#include <cmath>

#include "animation.hh"
#include "color.hh"
#include "control_flow.hh"
#include "gui_constants.hh"
#include "pointer.hh"
#include "widget.hh"

using namespace maf;

namespace automat::gui {

void Button::PointerOver(Pointer& pointer, animation::Display& display) {
  auto& animation_state = animation_state_ptr[display];
  animation_state.pointers_over++;
  pointer.PushIcon(Pointer::kIconHand);
  InvalidateDrawCache();
}

void Button::PointerLeave(Pointer& pointer, animation::Display& display) {
  animation_state_ptr[display].pointers_over--;
  pointer.PopIcon();
  InvalidateDrawCache();
}

namespace {

constexpr float kRadius = kMinimalTouchableSize / 2;

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

static float ShadowOffset(SkRRect& bounds) {
  return -Button::kPressOffset - (bounds.height() - kMinimalTouchableSize) / 4;
}

static float ShadowSigma(SkRRect& bounds) { return bounds.width() / 20; }

void Button::DrawButtonShadow(SkCanvas& canvas, SkColor bg) const {
  auto oval = RRect();
  float offset = ShadowOffset(oval), sigma = ShadowSigma(oval);
  oval.inset(kBorderWidth / 2, kBorderWidth / 2);
  oval.offset(0, offset);
  SkPaint shadow_paint;
  shadow_paint.setColor(color::AdjustLightness(bg, -40));
  shadow_paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, sigma, true));
  canvas.drawRRect(oval, shadow_paint);
}

maf::Optional<Rect> Button::TextureBounds(animation::Display*) const {
  auto rrect = RRect();
  float offset = ShadowOffset(rrect), sigma = ShadowSigma(rrect);
  Rect base_rect = rrect.rect();
  Rect shadow_rect = base_rect.sk.makeOffset(0, offset).makeOutset(sigma * 2, sigma * 2);
  base_rect.sk.join(shadow_rect.sk);
  return base_rect;
}

void Button::DrawButtonFace(DrawContext& ctx, SkColor bg, SkColor fg) const {
  auto& canvas = ctx.canvas;
  auto& display = ctx.display;
  auto& animation_state = animation_state_ptr[display];

  auto oval = RRect();
  oval.inset(kBorderWidth / 2, kBorderWidth / 2);
  float press_shift_y = PressRatio() * -kPressOffset;
  auto pressed_oval = oval.makeOffset(0, press_shift_y);
  float lightness_adjust = animation_state.highlight * 10;

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
}

static animation::Phase UpdateHighlight(DrawContext& ctx, Button::AnimationState& state) {
  return animation::LinearApproach(state.pointers_over ? 1 : 0, ctx.DeltaT(), 10, state.highlight);
}

void Button::PreDraw(DrawContext& ctx) const {
  auto bg = BackgroundColor();
  DrawButtonShadow(ctx.canvas, bg);
}

animation::Phase Button::Draw(DrawContext& ctx) const {
  auto& display = ctx.display;
  auto& animation_state = animation_state_ptr[display];
  auto phase = UpdateHighlight(ctx, animation_state);

  auto bg = BackgroundColor();
  auto fg = ForegroundColor(ctx);

  gui::Visitor visitor = [fg](Span<Widget*> children) {
    for (auto* child : children) {
      if (auto paint = PaintMixin::Get(child)) {
        paint->setColor(fg);
        paint->setAntiAlias(true);
      }
    }
    return ControlFlow::Continue;
  };
  const_cast<Button*>(this)->VisitChildren(visitor);

  DrawButtonFace(ctx, bg, fg);
  phase |= DrawChildren(ctx);
  return phase;
}

animation::Phase ToggleButton::Draw(DrawContext& ctx) const {
  auto& filling = filling_ptr[ctx.display];
  auto phase = animation::ExponentialApproach(Filled() ? 1 : 0, ctx.DeltaT(), 0.15, filling);
  phase |= DrawChildren(ctx);
  return phase;
}

animation::Phase ToggleButton::DrawChildCachced(DrawContext& ctx, const Widget& child) const {
  auto& canvas = ctx.canvas;
  auto& display = ctx.display;
  auto& filling = filling_ptr[display];
  auto on_widget = const_cast<ToggleButton*>(this)->OnWidget();
  if (filling >= 0.999) {
    if (&child == on_widget) {
      return on_widget->DrawCached(ctx);
    } else {
      return animation::Finished;
    }
  } else if (filling <= 0.001) {
    if (&child == off.get()) {
      return off->DrawCached(ctx);
    } else {
      return animation::Finished;
    }
  }

  auto pressed_outer_oval = on_widget->RRect();
  float baseline =
      pressed_outer_oval.rect().fTop * (1 - filling) + pressed_outer_oval.rect().fBottom * filling;
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

  if (&child == off.get()) {
    canvas.clipPath(waves_clip, SkClipOp::kDifference, true);
  } else {
    canvas.clipPath(waves_clip, SkClipOp::kIntersect, true);
  }

  return child.DrawCached(ctx);
}

void ToggleButton::PreDrawChildren(DrawContext& ctx) const {
  auto& canvas = ctx.canvas;
  auto& display = ctx.display;
  auto& filling = filling_ptr[display];

  auto on_widget = const_cast<ToggleButton*>(this)->OnWidget();

  canvas.saveLayerAlphaf(nullptr, filling);
  ctx.path.push_back(on_widget);
  on_widget->PreDraw(ctx);
  ctx.path.pop_back();
  canvas.restore();

  canvas.saveLayerAlphaf(nullptr, 1 - filling);
  ctx.path.push_back(off.get());
  off->PreDraw(ctx);
  ctx.path.pop_back();
  canvas.restore();
}

SkPath Button::Shape(animation::Display*) const { return SkPath::RRect(RRect()); }

struct ButtonAction : public Action {
  Button& button;
  ButtonAction(Pointer& pointer, Button& button) : Action(pointer), button(button) {}

  void Begin() override {
    button.Activate(pointer);
    button.press_action_count++;
  }

  void Update() override {}

  void End() override {
    button.press_action_count--;
    button.InvalidateDrawCache();
  }
};

std::unique_ptr<Action> Button::FindAction(Pointer& pointer, ActionTrigger pointer_button) {
  if (pointer_button == PointerButton::Left) {
    return std::make_unique<ButtonAction>(pointer, *this);
  }
  return nullptr;
}

SkRect Button::ChildBounds() const {
  if (child) return child->Shape(nullptr).getBounds();
  return SkRect::MakeEmpty();
}
SkMatrix Button::TransformToChild(const Widget& child, animation::Display*) const {
  SkRect rect = RRect().rect();
  if (child.CenteredAtZero()) return SkMatrix::Translate(-rect.center());
  SkRect child_bounds = ChildBounds();
  return SkMatrix::Translate(child_bounds.center() - rect.center());
}
}  // namespace automat::gui
