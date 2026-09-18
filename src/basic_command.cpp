// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
#include "basic_command.hpp"

#include "error.hpp"

using namespace std;

namespace automat {

std::unique_ptr<Action> BasicCommand::Table::DefaultActivate(Interface self, ui::Pointer& pointer,
                                                             Toy*) {
  cast<BasicCommand>(self).ScheduleRun();
  return std::make_unique<EmptyAction>(pointer);
}

constinit BasicCommand::Table kThisIsFine = [] {
  BasicCommand::Table t("This Is Fine");
  t.on_run = [](BasicCommand self) {
    ManipulateError(*self.object_ptr, [](Error& err) { err.Clear(); });
  };
  return t;
}();

}  // namespace automat
