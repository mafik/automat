// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include "on_off.hpp"

namespace automat {

Locked<Command> OnOff::Table::DefaultOnRun(Command self, Command::Table&) {
  auto on_off = cast<OnOff>(self);
  int idx = on_off.IsOn() ? on_off.table->turn_off_idx : on_off.table->turn_on_idx;
  return AcquireLocked(self.object_ptr->InterfaceFromIdx<Command>(idx));
}

}  // namespace automat
