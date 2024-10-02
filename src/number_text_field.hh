// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "base.hh"

namespace automat::gui {

struct NumberTextField : gui::TextField {
  maf::Str text;
  NumberTextField(float width);
  SkRRect ShapeRRect() const override;
  const SkPaint& GetBackgroundPaint() const override;
  void DrawBackground(gui::DrawContext&) const override;
  void DrawText(gui::DrawContext&) const override;
  Vec2 GetTextPos() const override;
  void SetNumber(double x);
  string_view Name() const override { return "NumberTextField"; }

  static void DrawBackground(gui::DrawContext&, SkRRect rrect);
};

}  // namespace automat::gui