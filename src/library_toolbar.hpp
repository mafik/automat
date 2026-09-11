#pragma once
// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT

#include "animation.hpp"
#include "base.hpp"
#include "pointer.hpp"
#include "vec.hpp"

namespace automat::ui {

constexpr float kToolbarIconSize = ui::kMinimalTouchableSize * 2;

struct PrototypeButton : Toy {
  Ptr<Object> proto;
  MortalPtr<Widget> proto_widget;
  float natural_width;
  mutable animation::SpringV2<float> width{kToolbarIconSize};

  PrototypeButton(Widget* parent, Ptr<Object>& proto)
      : Toy(parent, *proto, nullptr, proto->wake_counter), proto(proto) {}

  void Init();

  SkPath Shape() const override { return proto_widget->Shape(); }

  RRect CoarseBounds() const override { return proto_widget->CoarseBounds(); }

  Optional<Rect> DrawBounds() const override { return std::nullopt; }

  bool AllowChildPointerEvents(Widget& child) const override { return false; }

  Interface FindOption(Pointer&, ActionTrigger) override;

  StrView Name() const override { return "PrototypeButton"; }
};

struct Toolbar : ui::Widget, ui::PointerMoveCallback {
  Vec<Ptr<Object>> prototypes;
  Vec<std::unique_ptr<ui::PrototypeButton>> buttons;

  mutable int hovered_button = -1;

  Toolbar(ui::Widget* parent) : ui::Widget(parent) {
    parent->layers.OrderInside(this);
    shadow_elevation = 2_mm;
  }

  // This will clone the provided object and add it to the toolbar.
  void AddObjectPrototype(const Ptr<Object>&);

  StrView Name() const override;
  SkPath Shape() const override;
  Tock Tick(time::Timer&) override;
  void Draw(SkCanvas& canvas) const override;
  void UpdateChildTransform();
  float CalculateWidth() const;

  // If the object should be cached into a texture, return its bounds in local coordinates.
  Optional<Rect> DrawBounds() const override;

  void PointerEnter(ui::Pointer& pointer) override { StartWatching(pointer); }

  void PointerLeave(ui::Pointer& pointer) override {
    StopWatching(pointer);
    WakeAnimation();
  }
  void PointerMove(ui::Pointer&, Vec2 position) override { WakeAnimation(); }
};

}  // namespace automat::ui
