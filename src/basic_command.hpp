#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include "interface.hpp"
#include "tasks.hpp"

namespace automat {

struct BasicCommand : Interface {
  struct Table : Interface::Table {
    static bool classof(const Interface::Table* i) { return i->kind == Interface::kBasicCommand; }

    void (*on_run)(BasicCommand) = nullptr;

    static std::unique_ptr<Action> DefaultActivate(Interface, ui::Pointer&, Toy*);

    constexpr Table(StrView name) : Interface::Table(Interface::kBasicCommand, name) {
      activate = &DefaultActivate;
    }

    template <typename ImplT>
    constexpr void FillFrom() {
      Interface::Table::FillFrom<ImplT>();
      on_run = [](BasicCommand self) { static_cast<ImplT&>(self).OnRun(); };
    }
  };

  struct State {};

  INTERFACE_BOUND(BasicCommand, Interface)

  void ScheduleRun() const { (new RunTask(object_ptr->AcquireWeakPtr(), table_ptr))->Schedule(); }

  template <typename ImplT>
  struct Def : Interface::DefBase {
    using Impl = ImplT;
    using Bound = BasicCommand;

    static constexpr Table MakeTable() {
      Table t(ImplT::kName);
      t.FillFrom<ImplT>();
      return t;
    }

    inline constinit static Table tbl = MakeTable();

    ~Def() {}
  };
};

extern BasicCommand::Table kThisIsFine;

}  // namespace automat
