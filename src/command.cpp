// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
#include "command.hpp"

#include "error.hpp"

namespace automat {

bool Command::BeginRun() const {
  if (state->waiting) return false;
  if (!StartRunning()) return false;
  object_ptr->ClearOwnError();
  WakeToys();
  return true;
}

void Command::Done() const {
  state->waiting = true;
  WakeToys();
  if (!StopRunning()) state->waiting = false;
}

void Command::ContinueIfWaiting() const {
  if (!state->waiting.exchange(false)) return;
  if (!table->schedules_next) return;
  if (HasError(*object_ptr)) return;
  ScheduleNext(*object_ptr);
}

void Command::CancelWithoutNotify() const {
  state->waiting = false;
  if (!StopRunning()) return;
  if (auto on_cancel = table->on_cancel) on_cancel(*this);
}

}  // namespace automat
