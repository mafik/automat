// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
#include "command.hpp"

#include "error.hpp"

using namespace std;

namespace automat {

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
