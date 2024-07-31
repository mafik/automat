#pragma once

#include "animation.hh"
#include "base.hh"
#include "vec.hh"

namespace automat::gui {

constexpr float kToolbarIconSize = gui::kMinimalTouchableSize * 2;

struct PrototypeButton : Widget {
  const Object* proto;
  int pointers_over = 0;
  mutable animation::SpringV2<float> width{kToolbarIconSize};

  PrototypeButton(const Object* proto) : proto(proto) {}

  void Draw(DrawContext& ctx) const override { proto->Draw(ctx); }
  SkPath Shape(animation::Display*) const override { return proto->Shape(nullptr); }

  void PointerOver(Pointer& pointer, animation::Display&) override {
    pointer.PushIcon(Pointer::kIconHand);
    ++pointers_over;
  }

  void PointerLeave(Pointer& pointer, animation::Display&) override {
    --pointers_over;
    pointer.PopIcon();
  }

  std::unique_ptr<Action> ButtonDownAction(Pointer&, PointerButton btn) override;
};

}  // namespace automat::gui

namespace automat::library {

struct Toolbar : Object {
  static const Toolbar proto;

  maf::Vec<unique_ptr<Object>> prototypes;
  maf::Vec<unique_ptr<gui::PrototypeButton>> buttons;

  // This will clone the provided object and add it to the toolbar.
  void AddObjectPrototype(const Object*);

  maf::StrView Name() const override;
  std::unique_ptr<Object> Clone() const override;
  SkPath Shape(animation::Display* = nullptr) const override;
  void Draw(gui::DrawContext&) const override;
  ControlFlow VisitChildren(gui::Visitor& visitor) override;
  SkMatrix TransformToChild(const Widget& child, animation::Display*) const override;
  float CalculateWidth() const;
};

}  // namespace automat::library