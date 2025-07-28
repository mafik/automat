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
  NumberButton(gui::Widget& parent, SkPath shape);
  NumberButton(gui::Widget& parent, std::string text);
  void Activate(gui::Pointer&) override;
  StrView Name() const override { return "NumberButton"; }
  SkColor BackgroundColor() const override;
};

struct Number : Object, Object::FallbackWidget {
  double value;
  std::unique_ptr<NumberButton> digits[10];
  std::unique_ptr<NumberButton> dot;
  std::unique_ptr<NumberButton> backspace;
  std::unique_ptr<gui::NumberTextField> text_field;
  Number(gui::Widget& parent, double x = 0);
  string_view Name() const override;
  Ptr<Object> Clone() const override;
  string GetText() const override;
  void SetText(Location& error_context, string_view text) override;
  void Draw(SkCanvas&) const override;
  SkPath Shape() const override;
  void FillChildren(Vec<Widget*>& children) override;

  void SerializeState(Serializer& writer, const char* key) const override;
  void DeserializeState(Location& l, Deserializer& d) override;
};

}  // namespace automat::library