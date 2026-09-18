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

    constexpr Table(StrView name) : OnOff::Table(name, Interface::kCommand) {
      on_idle = [](Syncable self) { cast<Command>(self).ContinueIfWaiting(); };
    }

    template <typename ImplT>
    constexpr void FillFrom() {
      struct FullImpl : ImplT {
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
