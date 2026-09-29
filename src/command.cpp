// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
#include "command.hpp"

#include "error.hpp"

namespace automat {

std::unique_ptr<Action> Command::Table::DefaultActivate(Interface self, ui::Pointer& pointer,
                                                        Toy*) {
  self.object_ptr->ClearOwnError();
  cast<Command>(self).ScheduleRun();
  return std::make_unique<EmptyAction>(pointer);
}

Locked<Command> Command::NextCommand() const {
  if (table->next_idx == 0) return {};
  if (auto next = cast_if_present<Argument>(object_ptr->InterfaceFromIdx(table->next_idx))) {
    return FollowArgument(next);
  }
  return {};
}

void Command::Done() const {
  object_ptr->WakeToys();
  if (HasError(*object_ptr)) return;
  if (auto next = NextCommand()) next.ScheduleRun();
}

constinit Command::Table kThisIsFine = [] {
  Command::Table t("This Is Fine");
  t.on_run = [](Command self) -> Locked<Command> {
    ManipulateError(*self.object_ptr, [](Error& err) { err.Clear(); });
    return {};
  };
  return t;
}();

}  // namespace automat
