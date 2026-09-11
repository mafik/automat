#pragma once
// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT

#include "base.hpp"
#include "text_field.hpp"

namespace automat::ui {

struct NumberTextField : ui::TextField {
  NumberTextField(Widget* parent, Object& owner, automat::Text::Table& table, float width);
  SkRRect ShapeRRect() const override;
  const SkPaint& GetBackgroundPaint() const override;
  void DrawBackground(SkCanvas&) const override;
  void DrawText(SkCanvas&) const override;
  Vec2 GetTextPos() const override;
  string_view Name() const override { return "NumberTextField"; }

  static void DrawBackground(SkCanvas&, SkRRect rrect);
};

Str FormatNumber(double x, int max_digits = 5);

}  // namespace automat::ui