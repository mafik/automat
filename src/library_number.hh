#pragma once

#include "base.hh"
#include "gui_button.hh"
#include "number_text_field.hh"

namespace automat::library {

struct Number;

struct NumberButton : gui::Button {
  std::function<void(Location&)> activate;
  NumberButton(std::unique_ptr<Widget>&& child);
  void Activate(gui::Pointer&) override;
  maf::StrView Name() const override { return "NumberButton"; }
  SkColor BackgroundColor() const override;
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
  animation::Phase Draw(gui::DrawContext&) const override;
  SkPath Shape(animation::Display*) const override;
  ControlFlow VisitChildren(gui::Visitor& visitor) override;
  SkMatrix TransformToChild(const Widget& child, animation::Display*) const override;
  std::unique_ptr<Action> ButtonDownAction(gui::Pointer&, gui::PointerButton) override;

  void SerializeState(Serializer& writer, const char* key) const override;
  void DeserializeState(Location& l, Deserializer& d) override;
};

}  // namespace automat::library