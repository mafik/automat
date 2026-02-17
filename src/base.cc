// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "base.hh"

#include "tasks.hh"

using namespace std;

namespace automat {

Runnable::Table::Table(StrView name) : Syncable::Table(name, Interface::kRunnable) {
  can_sync = [](Syncable, Syncable other) -> bool {
    return other.table->kind == Interface::kRunnable;
  };
}

LongRunning::Table::Table(StrView name) : OnOff::Table(name, Interface::kLongRunning) {
  is_on = [](OnOff self) -> bool {
    return LongRunning(*self.obj, static_cast<LongRunning::Table&>(*self.table)).IsRunning();
  };
  on_turn_on = [](OnOff self) {
    if (auto* runnable = static_cast<Runnable::Table*>(self.obj->AsRunnable())) {
      Runnable(*self.obj, *runnable).ScheduleRun();
    }
  };
  on_turn_off = [](OnOff self) {
    LongRunning(*self.obj, static_cast<LongRunning::Table&>(*self.table)).Cancel();
  };
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
  task->DoneRunning(*obj);
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
    if (auto* lr_table = static_cast<LongRunning::Table*>(object->AsLongRunning())) {
      LongRunning lr(*object, *lr_table);
      if (lr.IsRunning()) {
        lr.Cancel();
      } else {
        Runnable(*object, *runnable).ScheduleRun();
      }
    } else {
      Runnable(*object, *runnable).ScheduleRun();
    }
  }
  return nullptr;
}

}  // namespace automat
