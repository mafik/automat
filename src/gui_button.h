#pragma once

#include "animation.h"
#include "product_ptr.h"
#include "widget.h"

namespace automaton::gui {

struct Button : Widget {
  Widget *parent_widget;
  std::unique_ptr<Widget> child;
  mutable product_ptr<animation::Approach> press_ptr;
  mutable product_ptr<animation::Approach> hover_ptr;
  mutable product_ptr<animation::Approach> toggle_ptr;
  int press_action_count = 0;
  bool toggled_on = false;

  Button(Widget *parent_widget, std::unique_ptr<Widget> &&child);
  Widget *ParentWidget() override;
  void PointerOver(Pointer &, animation::State &) override;
  void PointerLeave(Pointer &, animation::State &) override;
  void Draw(SkCanvas &, animation::State &animation_state) const override;
  SkPath Shape() const override;
  std::unique_ptr<Action> ButtonDownAction(Pointer &, PointerButton,
                                           vec2 contact_point) override;
  virtual void Activate() = 0;
  void Toggle();
};

} // namespace automaton::gui