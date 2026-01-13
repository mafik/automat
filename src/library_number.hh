// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "base.hh"
#include "number_text_field.hh"
#include "ui_button.hh"

namespace automat::library {

struct Number;

struct NumberButton : ui::Button {
  std::function<void(Location&)> activate;
  NumberButton(ui::Widget* parent, SkPath shape);
  NumberButton(ui::Widget* parent, std::string text);
  void Activate(ui::Pointer&) override;
  StrView Name() const override { return "NumberButton"; }
  SkColor BackgroundColor() const override;
};

struct Number : Object, Object::WidgetBase {
  double value;
  std::unique_ptr<NumberButton> digits[10];
  std::unique_ptr<NumberButton> dot;
  std::unique_ptr<NumberButton> backspace;
  std::unique_ptr<ui::NumberTextField> text_field;
  Number(ui::Widget* parent, double x = 0);
  string_view Name() const override;
  Ptr<Object> Clone() const override;
  string GetText() const override;
  void SetText(string_view text) override;
  void Draw(SkCanvas&) const override;
  SkPath Shape() const override;
  void FillChildren(Vec<Widget*>& children) override;
  bool CenteredAtZero() const override { return true; }

  void SerializeState(ObjectSerializer& writer) const override;
  void DeserializeState(ObjectDeserializer& d) override;
};

}  // namespace automat::library
