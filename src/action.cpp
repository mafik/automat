// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include "action.hpp"

#include "pointer.hpp"
#include "root_widget.hpp"

namespace automat {

Action::Action(ui::Pointer& pointer) : pointer(pointer) {
  pointer.root_widget.active_actions.Add(*this);
}

}  // namespace automat