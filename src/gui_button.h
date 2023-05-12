#pragma once

#include "animation.h"
#include "product_ptr.h"
#include "widget.h"

namespace automaton::gui {

struct Button : Widget {
  Widget *parent_widget;
  std::unique_ptr<Widget> child;
  mutable product_ptr<float> press_ptr;
  mutable product_ptr<animation::Approach> hover_ptr;
  mutable product_ptr<animation::Approach> filling_ptr;
  int press_action_count = 0;

  Button(Widget *parent_widget, std::unique_ptr<Widget> &&child);
  Widget *ParentWidget() override;
  void PointerOver(Pointer &, animation::State &) override;
  void PointerLeave(Pointer &, animation::State &) override;
  void Draw(SkCanvas &, animation::State &animation_state) const override;
  SkPath Shape() const override;
  std::unique_ptr<Action> ButtonDownAction(Pointer &, PointerButton) override;
  virtual void Activate() = 0;
  virtual bool Filled() const = 0;
};

} // namespace automaton::gui