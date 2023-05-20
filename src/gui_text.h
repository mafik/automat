#pragma once

#include "widget.h"

namespace automaton::gui {

struct Text : Widget {
  std::string text;
  Text(std::string_view text = "");
  SkPath Shape() const override;
  void Draw(SkCanvas &, animation::State &) const override;
};

} // namespace automaton::gui