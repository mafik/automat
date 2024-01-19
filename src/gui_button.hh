#pragma once

#include "animation.hh"
#include "product_ptr.hh"
#include "widget.hh"

namespace automat::gui {

struct Button : Widget {
  std::unique_ptr<Widget> child;
  mutable product_ptr<float> press_ptr;
  mutable product_ptr<animation::Approach> hover_ptr;
  int press_action_count = 0;
  SkColor color;

  Button(std::unique_ptr<Widget>&& child, SkColor color = 0xffd69d00);
  void PointerOver(Pointer&, animation::Context&) override;
  void PointerLeave(Pointer&, animation::Context&) override;
  void Draw(DrawContext&) const override;
  float Height() const;
  SkRRect RRect() const;
  SkPath Shape() const override;
  std::unique_ptr<Action> ButtonDownAction(Pointer&, PointerButton) override;
  virtual Vec2 Position() const { return Vec2(0, 0); }
  virtual void Activate(gui::Pointer&) {}

  void DrawButtonShadow(SkCanvas& canvas, SkColor bg) const;
  void DrawButtonFace(DrawContext&, SkColor bg, SkColor fg) const;
  void DrawButton(DrawContext&, SkColor bg) const;
};

struct ToggleButton : Button {
  mutable product_ptr<animation::Approach> filling_ptr;

  ToggleButton(std::unique_ptr<Widget>&& child, SkColor color = 0xffd69d00)
      : Button(std::move(child), color) {}
  void Draw(DrawContext&) const override;
  virtual bool Filled() const { return false; }
};

}  // namespace automat::gui