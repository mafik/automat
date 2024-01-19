#pragma once

#include "gui_button.hh"

namespace automat {

struct Location;

}  // namespace automat

namespace automat::gui {

struct RunButton : ToggleButton {
  Location* location;
  RunButton(Location* parent);
  void Activate(Pointer&) override;
  bool Filled() const override;
};

}  // namespace automat::gui