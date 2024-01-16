#pragma once

#include "base.hh"

namespace automat::gui {

struct NumberTextField : gui::TextField {
  Str text;
  NumberTextField(float width);
  SkRRect ShapeRRect() const override;
  const SkPaint& GetBackgroundPaint() const override;
  void DrawBackground(gui::DrawContext&) const override;
  void DrawText(gui::DrawContext&) const override;
  Vec2 GetTextPos() const override;
  void SetNumber(double x);
};

}  // namespace automat::gui