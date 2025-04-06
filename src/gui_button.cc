// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "gui_button.hh"

#include <include/core/SkBlurTypes.h>
#include <include/core/SkClipOp.h>
#include <include/core/SkColor.h>
#include <include/core/SkMaskFilter.h>
#include <include/core/SkRRect.h>
#include <include/effects/SkGradientShader.h>

#include <cmath>

#include "animation.hh"
#include "audio.hh"
#include "color.hh"
#include "embedded.hh"
#include "gui_constants.hh"
#include "pointer.hh"
#include "widget.hh"

using namespace maf;
using namespace std;

namespace automat::gui {

void Button::PointerOver(Pointer& pointer) {
  animation_state.pointers_over++;
  pointer.PushIcon(Pointer::kIconHand);
  WakeAnimation();
}

void Button::PointerLeave(Pointer& pointer) {
  animation_state.pointers_over--;
  pointer.PopIcon();
  WakeAnimation();
}

namespace {

constexpr float kRadius = kMinimalTouchableSize / 2;

}  // namespace

SkRRect Button::RRect() const {
  SkRect child_bounds = ChildBounds();
  float w, h;
  if (child->CenteredAtZero()) {
    w = std::max(kMinimalTouchableSize,
                 std::max(child_bounds.right(), -child_bounds.left()) * 2 + 2 * kMargin);
    h = std::max(kMinimalTouchableSize,
                 std::max(abs(child_bounds.bottom()), abs(child_bounds.top())) * 2 + 2 * kMargin);
  } else {
    w = std::max(kMinimalTouchableSize, child_bounds.width() + 2 * kMargin);
    h = std::max(kMinimalTouchableSize, child_bounds.height() + 2 * kMargin);
  }
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

maf::Optional<Rect> Button::TextureBounds() const {
  auto rrect = RRect();
  float offset = ShadowOffset(rrect), sigma = ShadowSigma(rrect);
  Rect base_rect = rrect.rect();
  Rect shadow_rect = base_rect.sk.makeOffset(0, offset).makeOutset(sigma * 2, sigma * 2);
  base_rect.sk.join(shadow_rect.sk);
  return base_rect;
}

void Button::DrawButtonFace(SkCanvas& canvas, SkColor bg, SkColor fg) const {
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

animation::Phase Button::Tick(time::Timer& timer) {
  auto phase = animation::LinearApproach(animation_state.pointers_over ? 1 : 0, timer.d, 10,
                                         animation_state.highlight);

  auto bg = BackgroundColor();
  auto fg = ForegroundColor();

  for (auto& child : Children()) {
    if (auto paint = PaintMixin::Get(child.get())) {
      if (paint->getColor() == fg) {
        continue;
      }
      paint->setColor(fg);
      paint->setAntiAlias(true);
    }
  }

  return phase;
}

void Button::PreDraw(SkCanvas& canvas) const {
  auto bg = BackgroundColor();
  DrawButtonShadow(canvas, bg);
}

void Button::Draw(SkCanvas& canvas) const {
  auto bg = BackgroundColor();
  auto fg = ForegroundColor();

  DrawButtonFace(canvas, bg, fg);
  DrawChildren(canvas);
}

animation::Phase ToggleButton::Tick(time::Timer& timer) {
  time_seconds = timer.NowSeconds();
  return animation::ExponentialApproach(Filled() ? 1 : 0, timer.d, 0.15, filling);
}

void ToggleButton::DrawChildCachced(SkCanvas& canvas, const Widget& child) const {
  auto on_widget = const_cast<ToggleButton*>(this)->OnWidget();
  if (filling >= 0.999) {
    if (&child == on_widget.get()) {
      return on_widget->DrawCached(canvas);
    } else {
      return;
    }
  } else if (filling <= 0.001) {
    if (&child == off.get()) {
      return off->DrawCached(canvas);
    } else {
      return;
    }
  }

  constexpr float kClipMargin = 0.5_mm;

  auto pressed_outer_oval = on_widget->RRect();
  float baseline =
      pressed_outer_oval.rect().fTop * (1 - filling) + pressed_outer_oval.rect().fBottom * filling;
  constexpr int n_points = 6;
  Vec2 points[n_points];
  constexpr float waving_x = kRadius / n_points / 2;
  float waving_y = waving_x * filling * (1 - filling) * 8;
  for (int i = 0; i < n_points; ++i) {
    float frac = i / float(n_points - 1);
    float a = (frac * 3 + time_seconds * 1) * 2 * M_PI;
    points[i].x = frac * (pressed_outer_oval.rect().fRight + kClipMargin) +
                  (1 - frac) * (pressed_outer_oval.rect().fLeft - kClipMargin) + cos(a) * waving_x;
    points[i].y = baseline + sin(a) * waving_y;
  }
  points[0].x = pressed_outer_oval.rect().fLeft - kClipMargin;
  points[n_points - 1].x = pressed_outer_oval.rect().fRight + kClipMargin;
  constexpr float wave_width = 2 * waving_x;
  SkPath waves_clip;
  waves_clip.moveTo(points[0].x, points[0].y);
  for (int i = 0; i < n_points - 1; ++i) {
    waves_clip.cubicTo(points[i].x + wave_width, points[i].y, points[i + 1].x - wave_width,
                       points[i + 1].y, points[i + 1].x, points[i + 1].y);
  }
  waves_clip.lineTo(pressed_outer_oval.rect().fRight + kClipMargin,
                    pressed_outer_oval.rect().fTop - Button::kPressOffset - kClipMargin);
  waves_clip.lineTo(pressed_outer_oval.rect().fLeft - kClipMargin,
                    pressed_outer_oval.rect().fTop - Button::kPressOffset - kClipMargin);
  waves_clip.close();

  canvas.save();
  if (&child == off.get()) {
    canvas.clipPath(waves_clip, SkClipOp::kDifference, true);
  } else {
    canvas.clipPath(waves_clip, SkClipOp::kIntersect, false);
  }

  if constexpr (false) {
    SkPaint debug_paint;
    debug_paint.setColor(SK_ColorRED);
    debug_paint.setStyle(SkPaint::kStroke_Style);
    canvas.drawPath(waves_clip, debug_paint);
  }

  child.DrawCached(canvas);

  canvas.restore();
}

void ToggleButton::PreDrawChildren(SkCanvas& canvas) const {
  auto on_widget = const_cast<ToggleButton*>(this)->OnWidget();

  canvas.saveLayerAlphaf(nullptr, filling);
  on_widget->PreDraw(canvas);
  canvas.restore();

  canvas.saveLayerAlphaf(nullptr, 1 - filling);
  off->PreDraw(canvas);
  canvas.restore();
}

SkPath Button::Shape() const { return SkPath::RRect(RRect()); }

struct ButtonAction : public Action {
  Button& button;
  ButtonAction(Pointer& pointer, Button& button) : Action(pointer), button(button) {
    audio::Play(embedded::assets_SFX_button_down_wav);
    button.press_action_count++;
    button.Activate(pointer);  // This may immediately end the action.
  }

  void Update() override {}

  ~ButtonAction() override {
    button.press_action_count--;
    button.WakeAnimation();
    audio::Play(embedded::assets_SFX_button_up_wav);
  }
};

std::unique_ptr<Action> Button::FindAction(Pointer& pointer, ActionTrigger pointer_button) {
  if (pointer_button == PointerButton::Left) {
    return std::make_unique<ButtonAction>(pointer, *this);
  }
  return nullptr;
}

SkRect Button::ChildBounds() const {
  if (child) return child->Shape().getBounds();
  return SkRect::MakeEmpty();
}

Button::Button(Ptr<Widget> child) : child(child) { UpdateChildTransform(); }

void Button::UpdateChildTransform() {
  Vec2 offset = RRect().rect().center();
  if (!child->CenteredAtZero()) {
    offset -= ChildBounds().center();
  }
  child->local_to_parent = SkM44::Translate(offset.x, offset.y);
}
}  // namespace automat::gui
