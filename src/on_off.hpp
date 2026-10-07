#pragma once
// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include "command.hpp"
#include "menu.hpp"
#include "pointer.hpp"

namespace automat {

struct OnOff : Command {
  struct Table : Command::Table {
    static bool classof(const Interface::Table* i) { return i->kind == Interface::kOnOff; }

    bool (*is_on)(OnOff) = nullptr;
    int turn_on_idx = 0;
    int turn_off_idx = 0;
    int syncable_idx = 0;

    static Locked<Command> DefaultOnRun(Command, Command::Table&);

    constexpr Table(StrView name) : Command::Table(name, Interface::kOnOff) {
      fill_menu = [](Interface self, Interface::Table& true_self, Menu& menu) {
        CommandFillMenu(self, true_self, menu);
        auto on_off = cast<OnOff>(self);
        auto& object = *self.object_ptr;
        menu.Place(ui::Dir::N, Interface(object, kToggle));
        menu.Place(ui::Dir::W, object.InterfaceFromIdx(on_off.table->turn_on_idx));
        menu.Place(ui::Dir::S, object.InterfaceFromIdx(on_off.table->turn_off_idx));
      };
      cursor = ui::Cursor::Hand;
    }

    template <typename ImplT>
    constexpr void FillFrom() {
      Interface::Table::FillFrom<ImplT>();
      is_on = [](OnOff self) { return static_cast<const ImplT&>(self).IsOn(); };
      turn_on_idx = ImplT::TurnOnIdx();
      turn_off_idx = ImplT::TurnOffIdx();
      if constexpr (requires { ImplT::SyncableIdx(); }) syncable_idx = ImplT::SyncableIdx();
      on_run = &DefaultOnRun;
    }
  };

  INTERFACE_BOUND(OnOff, Command)

  bool IsOn() const { return table->is_on(*this); }

  void TurnOn() const {
    RunLoop(AcquireLocked(object_ptr->InterfaceFromIdx<Command>(table->turn_on_idx)));
  }

  void TurnOff() const {
    RunLoop(AcquireLocked(object_ptr->InterfaceFromIdx<Command>(table->turn_off_idx)));
  }

  void Toggle() const { RunLoop(AcquireLocked<Command>(*this)); }

  void NotifyTurnedOn() const {
    if (auto suspend = Suspend(*object_ptr)) TurnOn();
  }

  void NotifyTurnedOff() const {
    if (auto suspend = Suspend(*object_ptr)) TurnOff();
  }

  // ImplT must provide:
  //   static constexpr StrView kName = "..."sv;
  //   bool IsOn() const;
  template <typename ImplT>
  struct Def : DefBase {
    using Impl = ImplT;
    using Bound = OnOff;

    static constexpr Table MakeTable() {
      Table t(ImplT::kName);
      t.FillFrom<ImplT>();
      return t;
    }

    inline constinit static Table tbl = MakeTable();
  };
};

}  // namespace automat
