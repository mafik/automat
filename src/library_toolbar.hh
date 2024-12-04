// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "animation.hh"
#include "base.hh"
#include "vec.hh"

namespace automat::gui {

constexpr float kToolbarIconSize = gui::kMinimalTouchableSize * 2;

struct PrototypeButton : Widget {
  std::shared_ptr<Object> proto;
  float natural_width;
  mutable animation::SpringV2<float> width{kToolbarIconSize};

  PrototypeButton(std::shared_ptr<Object>& proto) : proto(proto) {
    auto rect = proto->CoarseBounds().rect;
    natural_width =
        std::min<float>(kToolbarIconSize, rect.Width() * kToolbarIconSize / rect.Height());
    width.value = natural_width;
  }

  void Draw(SkCanvas& canvas) const override { DrawChildren(canvas); }
  SkPath Shape() const override { return proto->Shape(); }

  void FillChildren(maf::Vec<std::shared_ptr<Widget>>& children) override {
    children.push_back(proto);
  }

  void PointerOver(Pointer& pointer) override { pointer.PushIcon(Pointer::kIconHand); }

  void PointerLeave(Pointer& pointer) override { pointer.PopIcon(); }

  bool AllowChildPointerEvents(Widget& child) const override { return false; }

  std::unique_ptr<Action> FindAction(Pointer&, ActionTrigger btn) override;

  maf::StrView Name() const override { return "PrototypeButton"; }
};

}  // namespace automat::gui

namespace automat::library {

struct Toolbar : Object, gui::PointerMoveCallback {
  static std::shared_ptr<Toolbar> proto;

  maf::Vec<shared_ptr<Object>> prototypes;
  maf::Vec<shared_ptr<gui::PrototypeButton>> buttons;

  mutable int hovered_button = -1;

  // This will clone the provided object and add it to the toolbar.
  void AddObjectPrototype(const std::shared_ptr<Object>&);

  maf::StrView Name() const override;
  std::shared_ptr<Object> Clone() const override;
  SkPath Shape() const override;
  animation::Phase Update(time::Timer&) override;
  void Draw(SkCanvas& canvas) const override;
  void FillChildren(maf::Vec<std::shared_ptr<Widget>>& children) override;
  SkMatrix TransformToChild(const Widget& child) const override;
  float CalculateWidth() const;

  // If the object should be cached into a texture, return its bounds in local coordinates.
  maf::Optional<Rect> TextureBounds() const override;

  void PointerOver(gui::Pointer& pointer) override { StartWatching(pointer); }

  void PointerLeave(gui::Pointer& pointer) override {
    StopWatching(pointer);
    InvalidateDrawCache();
  }
  void PointerMove(gui::Pointer&, Vec2 position) override { InvalidateDrawCache(); }
};

}  // namespace automat::library