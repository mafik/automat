// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
#include "long_running.hpp"

namespace automat {

void LongRunning::CancelWithoutNotify() const {
  auto& task = state->task;
  if (task == nullptr) {
    ERROR << "LongRunning::Cancel called without a long_running_task";
    return;
  }
  if (auto on_cancel = table->on_cancel) on_cancel(*this);
  task.reset();
}

void LongRunning::Cancel() const {
  CancelWithoutNotify();
  NotifyTurnedOff();
}

void LongRunning::Done() const {
  auto& task = state->task;
  if (task == nullptr) {
    FATAL << "LongRunning::Done called while long_running_task == null.";
  }
  task->DoneRunning(*object_ptr);
  task.reset();
  NotifyTurnedOff();
}

}  // namespace automat