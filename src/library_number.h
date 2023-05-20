#pragma once

#include "base.h"

namespace automaton::library {

struct Number : Object {
  double value;
  gui::Button digits[10];
  gui::Button dot;
  gui::Button backspace;
  std::string text = "";
  gui::TextField text_field;
  Number(double x = 0);
  static const Number proto;
  string_view Name() const override;
  std::unique_ptr<Object> Clone() const override;
  string GetText() const override;
  void SetText(Location &error_context, string_view text) override;
  void Draw(SkCanvas &canvas, animation::State &animation_state) const override;
  SkPath Shape() const override;
  gui::VisitResult VisitImmediateChildren(gui::WidgetVisitor &visitor) override;
};

} // namespace automaton::library