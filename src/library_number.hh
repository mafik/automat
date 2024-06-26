#pragma once

#include "base.hh"
#include "gui_button.hh"
#include "number_text_field.hh"

namespace automat::library {

struct Number;

struct NumberButton : virtual gui::Button, gui::ChildButtonMixin {
  std::function<void(Location&)> activate;
  NumberButton(std::unique_ptr<Widget>&& child);
  void Draw(gui::DrawContext&) const override;
  void Activate(gui::Pointer&) override;
};

struct Number : Object {
  double value;
  NumberButton digits[10];
  NumberButton dot;
  NumberButton backspace;
  gui::NumberTextField text_field;
  Number(double x = 0);
  static const Number proto;
  string_view Name() const override;
  std::unique_ptr<Object> Clone() const override;
  string GetText() const override;
  void SetText(Location& error_context, string_view text) override;
  void Draw(gui::DrawContext&) const override;
  SkPath Shape() const override;
  ControlFlow VisitChildren(gui::Visitor& visitor) override;
  SkMatrix TransformToChild(const Widget& child, animation::Display*) const override;
  std::unique_ptr<Action> ButtonDownAction(gui::Pointer&, gui::PointerButton) override;
};

}  // namespace automat::library