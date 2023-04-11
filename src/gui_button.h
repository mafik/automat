#pragma once

#include "widget.h"

namespace automaton::gui {

struct Button : Widget {
  Widget *parent_widget;
  std::string label;

  Button(Widget *parent_widget, std::string label)
      : parent_widget(parent_widget), label(label) {}
  Widget *ParentWidget() override;
  void PointerOver(Pointer &, animation::State &) override;
  void PointerLeave(Pointer &, animation::State &) override;
  void Draw(SkCanvas &, animation::State &animation_state) const override;
  SkPath Shape() const override;
  std::unique_ptr<Action> ButtonDownAction(Pointer &, PointerButton,
                                           vec2 contact_point) override;
};

} // namespace automaton::gui