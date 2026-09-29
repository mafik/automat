#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include "error.hpp"
#include "interface.hpp"
#include "tasks.hpp"

namespace automat {

struct Command : Interface {
  struct Table : Interface::Table {
    static bool classof(const Interface::Table* i) { return i->kind == Interface::kCommand; }

    Locked<Command> (*on_run)(Command) = nullptr;
    int cancel_idx = 0;
    int next_idx = 0;

    static std::unique_ptr<Action> DefaultActivate(Interface, ui::Pointer&, Toy*);

    constexpr Table(StrView name) : Interface::Table(Interface::kCommand, name) {
      activate = &DefaultActivate;
    }

    template <typename ImplT>
    constexpr void FillFrom() {
      Interface::Table::FillFrom<ImplT>();
      if constexpr (requires(ImplT& i) {
                      { i.OnRun() } -> std::same_as<Locked<Command>>;
                    })
        on_run = [](Command self) { return static_cast<ImplT&>(self).OnRun(); };
      else
        on_run = [](Command self) -> Locked<Command> {
          static_cast<ImplT&>(self).OnRun();
          if (HasError(*self.object_ptr)) return {};
          return self.NextCommand();
        };
      if constexpr (requires { ImplT::CancelIdx(); }) cancel_idx = ImplT::CancelIdx();
      if constexpr (requires { ImplT::NextIdx(); }) next_idx = ImplT::NextIdx();
    }
  };

  struct State {};

  INTERFACE_BOUND(Command, Interface)

  Locked<Command> Run() const { return table->on_run(*this); }

  void ScheduleRun() const { (new RunTask(object_ptr->AcquireWeakPtr(), table_ptr))->Schedule(); }

  Locked<Command> NextCommand() const;

  void Done() const;

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
  };
};

extern Command::Table kThisIsFine;

}  // namespace automat
