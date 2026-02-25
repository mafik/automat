// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "base.hh"

#include "tasks.hh"

using namespace std;

namespace automat {

bool Runnable::Table::DefaultCanSync(Syncable, Syncable other) {
  return other.table->kind == Interface::kRunnable;
}

void LongRunning::Cancel() const {
  auto& task = state->task;
  if (task == nullptr) {
    ERROR << "LongRunning::Cancel called without a long_running_task";
    return;
  }
  if (auto on_cancel = table->on_cancel) on_cancel(*this);
  task.reset();
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

// --- RunOption ---

RunOption::RunOption(WeakPtr<Object> object, Runnable::Table& runnable)
    : TextOption("Run"), weak(std::move(object)), runnable(&runnable) {}
std::unique_ptr<Option> RunOption::Clone() const {
  return std::make_unique<RunOption>(weak, *runnable);
}
std::unique_ptr<Action> RunOption::Activate(ui::Pointer& pointer) const {
  if (auto object = weak.lock()) {
    if (auto lr = object->As<LongRunning>(); lr && lr.IsRunning()) {
      lr.Cancel();
    } else {
      Runnable(*object, *runnable).ScheduleRun();
    }
  }
  return nullptr;
}

}  // namespace automat
