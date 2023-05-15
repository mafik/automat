#pragma once

#include "gui_constants.h"
#include "location.h"
#include "widget.h"

namespace automaton::gui {

struct ConnectionWidget : Widget {
  constexpr static float kRadius = kMinimalTouchableSize / 2;
  Location *from;
  std::string label;
  ConnectionWidget(Location *from, std::string_view label);
  SkPath Shape() const override;
  void Draw(SkCanvas &, animation::State &) const override;

  vec2 Center() const;
};

} // namespace automaton::gui