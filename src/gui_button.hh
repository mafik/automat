#pragma once

#include <include/core/SkColor.h>

#include <memory>

#include "animation.hh"
#include "color.hh"
#include "double_ptr.hh"
#include "gui_constants.hh"
#include "widget.hh"

namespace automat::gui {

struct Button : Widget {
  constexpr static float kPressOffset = 0.2_mm;

  maf::DoublePtr<animation::Approach<>> hover_ptr;
  int press_action_count = 0;

  Button();
  void PointerOver(Pointer&, animation::Context&) override;
  void PointerLeave(Pointer&, animation::Context&) override;
  void Draw(DrawContext&) const override;
  float Height() const;
  virtual SkRRect RRect() const;
  SkPath Shape() const override;
  std::unique_ptr<Action> ButtonDownAction(Pointer&, PointerButton) override;
  virtual void Activate(gui::Pointer&) {}
  virtual Widget* Child() const { return nullptr; }
  SkRect ChildBounds() const;
  virtual SkColor ForegroundColor() const { return "#d69d00"_color; }
  virtual SkColor BackgroundColor() const { return SK_ColorWHITE; }
  virtual float PressRatio() const { return press_action_count ? 1 : 0; }

  void DrawButtonShadow(SkCanvas& canvas, SkColor bg) const;
  virtual void DrawButtonFace(DrawContext&, SkColor bg, SkColor fg) const;
  void DrawButton(DrawContext&, SkColor bg) const;
};

struct ChildButtonMixin : virtual Button {
  std::unique_ptr<Widget> child;
  ChildButtonMixin(std::unique_ptr<Widget>&& child) : child(std::move(child)) {}
  Widget* Child() const override { return child.get(); }
};

struct CircularButtonMixin : virtual Button {
  float radius;
  CircularButtonMixin(float radius) : radius(radius) {}
  SkRRect RRect() const override {
    SkRect oval = SkRect::MakeXYWH(0, 0, 2 * radius, 2 * radius);
    return SkRRect::MakeOval(oval);
  }
};

struct ToggleButton : virtual Button {
  maf::DoublePtr<animation::Approach<>> filling_ptr;

  ToggleButton() : Button() {}
  void Draw(DrawContext&) const override;
  virtual bool Filled() const { return false; }
};

}  // namespace automat::gui