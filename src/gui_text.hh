#pragma once

#include "widget.hh"

namespace automat::gui {

struct Text : Widget, PaintMixin {
  std::string text;
  Text(std::string_view text = "");
  SkPath Shape(animation::Display*) const override;
  void Draw(DrawContext&) const override;
  maf::StrView Name() const override { return "Text"; }
};

}  // namespace automat::gui