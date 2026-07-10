#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include <include/core/SkColor.h>

#include <memory>

#include "animation.hpp"
#include "key_button.hpp"
#include "math.hpp"
#include "ui_button.hpp"
#include "units.hpp"
#include "widget.hpp"

namespace automat::ui {

struct Rocker : Widget {
  constexpr static auto kBounds = RRect::MakeSimple(Rect::MakeAtZero(8_mm, 16_mm), 1_mm);
  constexpr static SkColor4f kColor = library::kKeyDisabledColor;

  // The rocker is divided into three "faces":
  // - top - the part that's above the rocker face
  // - face - the part of the rocker that has labels, it's curved in the middle which separates it
  //   into upper & lower portions
  // - bottom - the part below the rocker face

  // Control points along the vertical gradient
  enum ControlPoint {
    TOP_HORIZON,  // pos always 0
    TOP_FRONT_EDGE,
    FRONT_TOP_EDGE,
    FRONT_UPPER_CURVE,
    FRONT_LOWER_CURVE,
    FRONT_BOTTOM_EDGE,
    BOTTOM_FRONT_EDGE,
    BOTTOM_HORIZON,  // pos always 1
    NUM_VERTICAL_POINTS,
  };
  SkColor4f colors[NUM_VERTICAL_POINTS];
  float positions[NUM_VERTICAL_POINTS];

  bool on = false;
  animation::SpringV2<float> state = 0;
  Clickable clickable;
  std::unique_ptr<Widget> on_icon;
  std::unique_ptr<Widget> off_icon;

  Rocker(Widget* parent);

  void SetOn(bool);

  Tock Tick(time::Timer&) override;
  void Draw(SkCanvas&) const override;
  SkPath Shape() const override;
  void PointerHover(Pointer& p) override { clickable.PointerHover(p); }
  void PointerUnhover(Pointer& p) override { clickable.PointerUnhover(p); }
  std::unique_ptr<Action> FindAction(Pointer& p, ActionTrigger a) override {
    return clickable.FindAction(p, a);
  }
  bool AllowChildPointerEvents(Widget& child) const override { return false; }
  RRect CoarseBounds() const override { return kBounds; }
  Optional<Rect> TextureBounds() const override {
    auto bounds = kBounds.rect;
    bounds.left -= 2_mm;
    bounds.right += 2_mm;
    bounds.bottom -= 3_mm;
    return bounds;
  }
};

}  // namespace automat::ui
