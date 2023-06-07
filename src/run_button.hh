#pragma once

#include "gui_button.hh"

namespace automat {

struct Location;

}  // namespace automat

namespace automat::gui {

struct RunButton : Button {
  Location* location;
  RunButton(Location* parent);
  void Activate() override;
  bool Filled() const override;
};

}  // namespace automat::gui