#pragma once

#include "base.hh"

namespace automat::library {

struct Number;

struct NumberButton : gui::Button {
  std::function<void()> activate;
  NumberButton(std::unique_ptr<Widget>&& child);
  void Draw(gui::DrawContext&) const override;
  void Activate() override;
};

struct NumberTextField : gui::TextField {
  NumberTextField(Number&);
  SkRRect ShapeRRect() const override;
  const SkPaint& GetBackgroundPaint() const override;
};

struct Number : Object {
  double value;
  NumberButton digits[10];
  NumberButton dot;
  NumberButton backspace;
  std::string text;
  NumberTextField text_field;
  Number(double x = 0);
  static const Number proto;
  string_view Name() const override;
  std::unique_ptr<Object> Clone() const override;
  string GetText() const override;
  void SetText(Location& error_context, string_view text) override;
  void Draw(gui::DrawContext&) const override;
  SkPath Shape() const override;
  MaybeStop VisitChildren(gui::Visitor& visitor) override;
  SkMatrix TransformToChild(const Widget& child, animation::Context&) const override;
  std::unique_ptr<Action> ButtonDownAction(gui::Pointer&, gui::PointerButton) override;
};

}  // namespace automat::library