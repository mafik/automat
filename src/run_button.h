#pragma once

#include "gui_button.h"

namespace automaton {

struct Location;

} // namespace automaton

namespace automaton::gui {

struct RunButton : Button {
  RunButton(Location *parent);
  void Activate() override;
};

} // namespace automaton::gui