// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "base.hh"

#include "tasks.hh"

using namespace std;

namespace automat {

RunOption::RunOption(WeakPtr<Object> object, Runnable& runnable)
    : TextOption("Run"), weak(std::move(object)), runnable(&runnable) {}
std::unique_ptr<Option> RunOption::Clone() const {
  return std::make_unique<RunOption>(weak, *runnable);
}
std::unique_ptr<Action> RunOption::Activate(ui::Pointer& pointer) const {
  if (auto object = weak.lock()) {
    if (auto long_running = object->AsLongRunning(); long_running && long_running->IsRunning()) {
      long_running->Cancel();
    } else {
      runnable->ScheduleRun(*object);
    }
  }
  return nullptr;
}

void LongRunning::Done(Object& object) {
  if (long_running_task == nullptr) {
    FATAL << "LongRunning::Done called while long_running_task == null.";
  }
  long_running_task->DoneRunning(object);
  long_running_task.reset();
  NotifyTurnedOff();
}

void LongRunning::OnTurnOn() {
  auto object = OnFindRunnable();
  if (object == nullptr) {
    ERROR << "LongRunning::OnFindRunnable didn't return any Object!";
    return;
  }
  if (auto* runnable = object->AsRunnable()) {
    runnable->ScheduleRun(*object);
  }
}

}  // namespace automat
