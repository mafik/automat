// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "base.hpp"

#include "error.hpp"
#include "tasks.hpp"

using namespace std;

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

std::unique_ptr<Action> Command::Table::DefaultActivate(Interface self, ui::Pointer& pointer,
                                                        Toy*) {
  cast<Command>(self).ScheduleRun();
  return std::make_unique<EmptyAction>(pointer);
}

constinit Command::Table kThisIsFine = [] {
  Command::Table t("This Is Fine");
  t.schedules_next = false;
  t.on_run = [](Command self, std::unique_ptr<RunTask>&) {
    ManipulateError(*self.object_ptr, [](Error& err) { err.Clear(); });
  };
  return t;
}();
}  // namespace automat
