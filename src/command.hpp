#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include "interface.hpp"
#include "tasks.hpp"

namespace automat {

struct Command : Interface {
  // What to do with a command that arrives while the object's LongRunning is
  // active. An object's starting command is inhibited (a running thing is not
  // started twice); a command that performs one bounded unit of work (pull one
  // buffer, decode one frame) is delivered.
  enum WhileLongRunning : bool { kDeliver, kInhibit };

  struct Table : Interface::Table {
    static bool classof(const Interface::Table* i) { return i->kind == Interface::kCommand; }

    void (*on_run)(Command, std::unique_ptr<RunTask>&) = nullptr;

    WhileLongRunning while_long_running;
    bool schedules_next = true;

    static std::unique_ptr<Action> DefaultActivate(Interface, ui::Pointer&, Toy*);

    constexpr Table(StrView name, WhileLongRunning while_long_running = kDeliver)
        : Interface::Table(Interface::kCommand, name), while_long_running(while_long_running) {
      activate = &DefaultActivate;
    }

    template <typename ImplT>
    constexpr void FillFrom() {
      Interface::Table::FillFrom<ImplT>();
      on_run = [](Command self, std::unique_ptr<RunTask>& t) {
        static_cast<ImplT&>(self).OnRun(t);
      };
      if constexpr (requires { ImplT::kSchedulesNext; }) schedules_next = ImplT::kSchedulesNext;
    }
  };

  struct State {};

  INTERFACE_BOUND(Command, Interface)

  void Run(std::unique_ptr<RunTask>& run_task) const { table->on_run(*this, run_task); }

  void ScheduleRun(Interface source = {}) const {
    auto* task = new RunTask(object_ptr->AcquireWeakPtr(), table_ptr);
    if (source) {
      task->source = source.object_ptr->AcquireWeakPtr();
      task->source_interface = source.table_ptr;
    }
    task->Schedule();  // steals the ownership of the pointer
  }

  // ImplT must provide:
  //   static constexpr StrView kName = "..."sv;
  //   static constexpr int Offset();  // offsetof(Parent, def_member)
  //   void OnRun(std::unique_ptr<RunTask>&);
  template <typename ImplT>
  struct Def : Interface::DefBase {
    using Impl = ImplT;
    using Bound = Command;

    static constexpr Table MakeTable() {
      Table t(ImplT::kName);
      t.FillFrom<ImplT>();
      return t;
    }

    inline constinit static Table tbl = MakeTable();

    ~Def() {}
  };
};

extern Command::Table kThisIsFine;

}  // namespace automat