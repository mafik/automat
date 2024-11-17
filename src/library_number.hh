// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "base.hh"
#include "gui_button.hh"
#include "number_text_field.hh"

namespace automat::library {

struct Number;

struct NumberButton : gui::Button {
  std::function<void(Location&)> activate;
  NumberButton(std::shared_ptr<Widget> child);
  NumberButton(std::string text);
  void Activate(gui::Pointer&) override;
  maf::StrView Name() const override { return "NumberButton"; }
  SkColor BackgroundColor() const override;
};

struct Number : Object {
  double value;
  std::shared_ptr<NumberButton> digits[10];
  std::shared_ptr<NumberButton> dot;
  std::shared_ptr<NumberButton> backspace;
  std::shared_ptr<gui::NumberTextField> text_field;
  Number(double x = 0);
  static std::shared_ptr<Number> proto;
  string_view Name() const override;
  std::shared_ptr<Object> Clone() const override;
  string GetText() const override;
  void SetText(Location& error_context, string_view text) override;
  animation::Phase Draw(gui::DrawContext&) const override;
  SkPath Shape() const override;
  ControlFlow VisitChildren(gui::Visitor& visitor) override;
  SkMatrix TransformToChild(const Widget& child, animation::Display*) const override;

  void SerializeState(Serializer& writer, const char* key) const override;
  void DeserializeState(Location& l, Deserializer& d) override;
};

}  // namespace automat::library