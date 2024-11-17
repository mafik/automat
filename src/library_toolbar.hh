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

  animation::Phase Draw(DrawContext& ctx) const override { return DrawChildren(ctx); }
  SkPath Shape() const override { return proto->Shape(); }

  ControlFlow VisitChildren(gui::Visitor& visitor) override {
    const std::shared_ptr<Widget>& proto_widget = proto;
    return visitor(maf::SpanOfArr(const_cast<std::shared_ptr<Widget>*>(&proto_widget), 1));
  }

  void PointerOver(Pointer& pointer) override { pointer.PushIcon(Pointer::kIconHand); }

  void PointerLeave(Pointer& pointer) override { pointer.PopIcon(); }

  ControlFlow PointerVisitChildren(Visitor&) override { return ControlFlow::Continue; }

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
  animation::Phase Draw(gui::DrawContext&) const override;
  ControlFlow VisitChildren(gui::Visitor& visitor) override;
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