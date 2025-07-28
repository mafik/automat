// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "animation.hh"
#include "base.hh"
#include "pointer.hh"
#include "vec.hh"

namespace automat::gui {

constexpr float kToolbarIconSize = gui::kMinimalTouchableSize * 2;

struct PrototypeButton : Widget {
  Ptr<Object> proto;
  Widget* proto_widget;
  float natural_width;
  mutable animation::SpringV2<float> width{kToolbarIconSize};
  Optional<Pointer::IconOverride> hand_icon;

  PrototypeButton(Widget* parent, Ptr<Object>& proto) : gui::Widget(parent), proto(proto) {}

  void Init() {
    proto_widget = &Widget::ForObject(*proto, this);
    auto rect = proto_widget->CoarseBounds().rect;
    natural_width =
        std::min<float>(kToolbarIconSize, rect.Width() * kToolbarIconSize / rect.Height());
    width.value = natural_width;
  }

  SkPath Shape() const override { return proto_widget->Shape(); }

  RRect CoarseBounds() const override { return proto_widget->CoarseBounds(); }

  Optional<Rect> TextureBounds() const override { return proto_widget->TextureBounds(); }

  void FillChildren(Vec<Widget*>& children) override { children.push_back(proto_widget); }

  void PointerOver(Pointer& pointer) override { hand_icon.emplace(pointer, Pointer::kIconHand); }

  void PointerLeave(Pointer& pointer) override { hand_icon.reset(); }

  bool AllowChildPointerEvents(Widget& child) const override { return false; }

  std::unique_ptr<Action> FindAction(Pointer&, ActionTrigger btn) override;

  StrView Name() const override { return "PrototypeButton"; }
};

struct Toolbar : gui::Widget, gui::PointerMoveCallback {
  Vec<Ptr<Object>> prototypes;
  Vec<std::unique_ptr<gui::PrototypeButton>> buttons;

  mutable int hovered_button = -1;

  Toolbar(gui::Widget* parent) : gui::Widget(parent) {}

  // This will clone the provided object and add it to the toolbar.
  void AddObjectPrototype(const Ptr<Object>&);

  StrView Name() const override;
  SkPath Shape() const override;
  animation::Phase Tick(time::Timer&) override;
  void Draw(SkCanvas& canvas) const override;
  void FillChildren(Vec<Widget*>& children) override;
  void UpdateChildTransform();
  float CalculateWidth() const;

  // If the object should be cached into a texture, return its bounds in local coordinates.
  Optional<Rect> TextureBounds() const override;

  void PointerOver(gui::Pointer& pointer) override { StartWatching(pointer); }

  void PointerLeave(gui::Pointer& pointer) override {
    StopWatching(pointer);
    WakeAnimation();
  }
  void PointerMove(gui::Pointer&, Vec2 position) override { WakeAnimation(); }
};

}  // namespace automat::gui