#pragma once

#include "widget.hh"

namespace automat::gui {

struct Text : Widget {
  std::string text;
  Text(std::string_view text = "");
  SkPath Shape() const override;
  void Draw(DrawContext&) const override;
};

}  // namespace automat::gui