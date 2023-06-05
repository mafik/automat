#pragma once

#include "gui_button.h"

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