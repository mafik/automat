#pragma once

#include "animation.hh"
#include "base.hh"
#include "vec.hh"

namespace automat::gui {

constexpr float kToolbarIconSize = gui::kMinimalTouchableSize * 2;

struct PrototypeButton : Widget {
  const Object* proto;
  float natural_width;
  mutable animation::SpringV2<float> width{kToolbarIconSize};

  PrototypeButton(const Object* proto) : proto(proto) {
    auto rect = proto->CoarseBounds(nullptr).rect;
    natural_width =
        std::min<float>(kToolbarIconSize, rect.Width() * kToolbarIconSize / rect.Height());
    width.value = natural_width;
  }

  animation::Phase Draw(DrawContext& ctx) const override { return proto->Draw(ctx); }
  SkPath Shape(animation::Display*) const override { return proto->Shape(nullptr); }

  ControlFlow VisitChildren(gui::Visitor& visitor) override {
    Widget* arr[] = {const_cast<Object*>(proto)};
    return visitor(arr);
  }

  void PointerOver(Pointer& pointer, animation::Display&) override {
    pointer.PushIcon(Pointer::kIconHand);
  }

  void PointerLeave(Pointer& pointer, animation::Display&) override { pointer.PopIcon(); }

  ControlFlow PointerVisitChildren(Visitor&) override { return ControlFlow::Continue; }

  std::unique_ptr<Action> ButtonDownAction(Pointer&, PointerButton btn) override;

  maf::StrView Name() const override { return "PrototypeButton"; }
};

}  // namespace automat::gui

namespace automat::library {

struct Toolbar : Object, gui::PointerMoveCallback {
  static const Toolbar proto;

  maf::Vec<unique_ptr<Object>> prototypes;
  maf::Vec<unique_ptr<gui::PrototypeButton>> buttons;

  // This will clone the provided object and add it to the toolbar.
  void AddObjectPrototype(const Object*);

  maf::StrView Name() const override;
  std::unique_ptr<Object> Clone() const override;
  SkPath Shape(animation::Display* = nullptr) const override;
  animation::Phase Draw(gui::DrawContext&) const override;
  ControlFlow VisitChildren(gui::Visitor& visitor) override;
  SkMatrix TransformToChild(const Widget& child, animation::Display*) const override;
  float CalculateWidth() const;

  // If the object should be cached into a texture, return its bounds in local coordinates.
  maf::Optional<Rect> TextureBounds(animation::Display*) const override;

  void PointerOver(gui::Pointer& pointer, animation::Display&) override { StartWatching(pointer); }

  void PointerLeave(gui::Pointer& pointer, animation::Display&) override {
    StopWatching(pointer);
    InvalidateDrawCache();
  }
  void PointerMove(gui::Pointer&, Vec2 position) override { InvalidateDrawCache(); }
};

}  // namespace automat::library