#pragma once

#include "gui_button.hh"

namespace automat {

struct Location;

}  // namespace automat

namespace automat::gui {

struct RunButton : ToggleButton, CircularButtonMixin {
  Location* location;
  RunButton(Location* parent = nullptr, float radius = kMinimalTouchableSize / 2);
  void Activate(Pointer&) override;
  bool Filled() const override;
};

}  // namespace automat::gui