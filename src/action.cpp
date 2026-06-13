// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include "action.hpp"

#include "pointer.hpp"
#include "root_widget.hpp"
#include "vec.hpp"

namespace automat {

Action::Action(ui::Pointer& pointer) : pointer(pointer) {
  pointer.root_widget.active_actions.push_back(this);
}

Action::~Action() { FastRemove(pointer.root_widget.active_actions, this); }

}  // namespace automat