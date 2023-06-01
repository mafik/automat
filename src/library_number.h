#pragma once

#include "base.h"

namespace automat::library {

struct NumberButton : gui::Button {
  std::function<void()> activate;
  NumberButton(Widget *parent, std::unique_ptr<Widget> &&child);
  void Draw(SkCanvas &, animation::State &animation_state) const override;
  void Activate() override;
};

struct Number : Object {
  double value;
  NumberButton digits[10];
  NumberButton dot;
  NumberButton backspace;
  std::string text;
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
  std::unique_ptr<Action> ButtonDownAction(gui::Pointer &,
                                           gui::PointerButton) override;
};

} // namespace automat::library