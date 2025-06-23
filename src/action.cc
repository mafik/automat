// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include "action.hh"

#include "pointer.hh"
#include "root_widget.hh"
#include "vec.hh"

namespace automat {

Action::Action(gui::Pointer& pointer) : pointer(pointer) {
  pointer.root_widget.active_actions.push_back(this);
}

Action::~Action() { FastRemove(pointer.root_widget.active_actions, this); }

}  // namespace automat