// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
#include "command.hpp"

#include "error.hpp"
#include "menu.hpp"
#include "object.hpp"
#include "tasks.hpp"

namespace automat {

std::unique_ptr<Action> Command::Table::CommandActivate(Span<Interface> chain, ui::Pointer& pointer,
                                                        Toy*) {
  auto self = chain.back();
  self.object_ptr->ClearOwnError();
  cast<Command>(self).ScheduleRun();
  return std::make_unique<EmptyAction>(pointer);
}

void Command::Table::CommandFillMenu(Interface self, Interface::Table&, Menu& menu) {
  Object& object = *self.object_ptr;
  menu.Place(ui::Dir::N, Interface(object, kRun));
  Object::TocGuard guard(object);
  if (object.toc.load(std::memory_order_acquire)->SlotOf(self.table_ptr)) {
    menu.Place(ui::Dir::E, Interface(object, kSync));
  }
}

void Command::ScheduleRun() const {
  (new RunTask(object_ptr->AcquireWeakPtr(), table_ptr))->Schedule();
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

void RunCascade(Locked<Command> cmd) {
  while (cmd) cmd = cmd.Run();
}

constinit Command::Table kThisIsFine = [] {
  Command::Table t("This Is Fine");
  t.on_run = [](Command self, Command::Table&) -> Locked<Command> {
    ManipulateError(*self.object_ptr, [](Error& err) { err.Clear(); });
    return {};
  };
  return t;
}();

static std::unique_ptr<Action> RunCaller(Span<Interface> chain, ui::Pointer& pointer, Toy*) {
  if (chain.size() < 2) return nullptr;
  auto caller = chain[chain.size() - 2];
  caller.object_ptr->ClearOwnError();
  cast<Command>(caller).ScheduleRun();
  return std::make_unique<EmptyAction>(pointer);
}

constinit Interface::Table kRun = [] {
  Interface::Table t(Interface::kMenu, "Run");
  t.activate = &RunCaller;
  return t;
}();

constinit Interface::Table kToggle = [] {
  Interface::Table t(Interface::kMenu, "Toggle");
  t.activate = &RunCaller;
  return t;
}();

}  // namespace automat
