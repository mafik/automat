// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "base.hh"

namespace automat::gui {

struct NumberTextField : gui::TextField {
  Str text;
  NumberTextField(float width);
  SkRRect ShapeRRect() const override;
  const SkPaint& GetBackgroundPaint() const override;
  void DrawBackground(SkCanvas&) const override;
  void DrawText(SkCanvas&) const override;
  Vec2 GetTextPos() const override;
  void SetNumber(double x);
  string_view Name() const override { return "NumberTextField"; }

  static void DrawBackground(SkCanvas&, SkRRect rrect);
};

}  // namespace automat::gui