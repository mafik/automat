#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include "on_off.hpp"

namespace automat {

struct Command : OnOff {
  struct Table : OnOff::Table {
    static bool classof(const Interface::Table* i) { return i->kind == Interface::kCommand; }

    void (*on_cancel)(Command) = nullptr;

    bool schedules_next = true;

    BasicCommand::Table turn_on;
    BasicCommand::Table turn_off;

    constexpr Table(StrView name)
        : OnOff::Table(name, Interface::kCommand), turn_on("Turn on"), turn_off("Turn off") {
      fill_menu = [](Interface self, Menu& menu) {
        Syncable::Table::DefaultFillMenu(self, menu);
        auto command = cast<Command>(self);
        menu.Place(ui::Dir::N,
                   Interface(self.object_ptr,
                             command.IsOn() ? &command.table->turn_off : &command.table->turn_on));
      };
      on_idle = [](Syncable self) { cast<Command>(self).ContinueIfWaiting(); };
      turn_on.activate = [](Interface self, ui::Pointer& pointer,
                            automat::Toy*) -> std::unique_ptr<Action> {
        Command(self.object_ptr, OUTER_PTR(Command::Table, turn_on, self.table_ptr)).TurnOn();
        return std::make_unique<EmptyAction>(pointer);
      };
      turn_off.activate = [](Interface self, ui::Pointer& pointer,
                             automat::Toy*) -> std::unique_ptr<Action> {
        Command(self.object_ptr, OUTER_PTR(Command::Table, turn_off, self.table_ptr)).TurnOff();
        return std::make_unique<EmptyAction>(pointer);
      };
    }

    template <typename ImplT>
    constexpr void FillFrom() {
      struct FullImpl : ImplT {
        static constexpr int TurnOnIdx() { return 0; }
        static constexpr int TurnOffIdx() { return 0; }
        bool IsOn() const { return this->IsRunning(); }
        void OnTurnOn() {
          if constexpr (requires(ImplT& i) { i.OnRun(); }) {
            if (this->BeginRun()) this->OnRun();
          }
        }
        void OnTurnOff() { this->CancelWithoutNotify(); }
        void OnUnsync() {
          if constexpr (requires(ImplT& i) { i.OnUnsync(); }) ImplT::OnUnsync();
          this->ContinueIfWaiting();
        }
      };
      OnOff::Table::FillFrom<FullImpl>();
      on_turn_on = [](OnOff self) { static_cast<FullImpl&>(self).OnTurnOn(); };
      on_turn_off = [](OnOff self) { static_cast<FullImpl&>(self).OnTurnOff(); };
      turn_on.cursor = turn_off.cursor = cursor;
      if constexpr (requires { ImplT::kSchedulesNext; }) schedules_next = ImplT::kSchedulesNext;
      if constexpr (requires(ImplT& i) { i.OnCancel(); })
        on_cancel = [](Command self) { static_cast<ImplT&>(self).OnCancel(); };
    }
  };

  struct State : OnOff::State {
    std::atomic<bool> waiting = false;

    State() = default;
    State(const State& other) : OnOff::State(other) {}
  };

  INTERFACE_BOUND(Command, OnOff)

  void ScheduleRun() const { (new RunTask(object_ptr->AcquireWeakPtr(), table_ptr))->Schedule(); }

  bool BeginRun() const;

  void Done() const;

  void ContinueIfWaiting() const;

  void CancelWithoutNotify() const;

  void Cancel() const { TurnOff(); }

  template <typename ImplT>
  struct Def : State, Interface::DefBase {
    using Impl = ImplT;
    using Bound = Command;

    static constexpr Table MakeTable() {
      Table t(ImplT::kName);
      t.FillFrom<ImplT>();
      return t;
    }

    inline constinit static Table tbl = MakeTable();

    ~Def() {
      if (IsRunning()) {
        Bind().Cancel();
      }
      if (IsSource() || HasGear()) {
        Bind().Unsync();
      }
    }
  };
};

}  // namespace automat
