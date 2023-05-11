#pragma once

#include "gui_button.h"

namespace automaton {

struct Location;

} // namespace automaton

namespace automaton::gui {

struct RunButton : Button {
  Location *location;
  RunButton(Location *parent);
  void Activate() override;
  bool Filled() const override;
};

} // namespace automaton::gui