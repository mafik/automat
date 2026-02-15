// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "base.hh"

#include "tasks.hh"

using namespace std;

namespace automat {

Runnable::Runnable(StrView name) : Syncable(name, Interface::kRunnable) {
  can_sync = [](const Syncable&, const Syncable& other) -> bool {
    return other.kind == Interface::kRunnable;
  };
}

LongRunning::LongRunning(StrView name) : OnOff(name, Interface::kLongRunning) {
  is_on = [](const OnOff& iface, const Object& obj) -> bool {
    return static_cast<const LongRunning&>(iface).IsRunning(obj);
  };
  on_turn_on = [](const OnOff&, Object& obj) {
    if (auto* runnable = obj.AsRunnable()) {
      runnable->ScheduleRun(obj);
    }
  };
  on_turn_off = [](const OnOff& iface, Object& obj) {
    static_cast<const LongRunning&>(iface).Cancel(obj);
  };
}

void LongRunning::Done(Object& self) const {
  auto& task = get_task(self);
  if (task == nullptr) {
    FATAL << "LongRunning::Done called while long_running_task == null.";
  }
  task->DoneRunning(self);
  task.reset();
  NotifyTurnedOff(self);
}

// --- RunOption ---

RunOption::RunOption(WeakPtr<Object> object, const Runnable& runnable)
    : TextOption("Run"), weak(std::move(object)), runnable(&runnable) {}
std::unique_ptr<Option> RunOption::Clone() const {
  return std::make_unique<RunOption>(weak, *runnable);
}
std::unique_ptr<Action> RunOption::Activate(ui::Pointer& pointer) const {
  if (auto object = weak.lock()) {
    if (auto* long_running = object->AsLongRunning();
        long_running && long_running->IsRunning(*object)) {
      long_running->Cancel(*object);
    } else {
      runnable->ScheduleRun(*object);
    }
  }
  return nullptr;
}

}  // namespace automat
