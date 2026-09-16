#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include "base.hpp"
#include "on_off.hpp"

namespace automat {

struct LongRunning : OnOff {
  struct State : OnOff::State {
    std::unique_ptr<RunTask> task;
  };

  struct Table : OnOff::Table {
    static bool classof(const Interface::Table* i) { return i->kind == Interface::kLongRunning; }

    void (*on_cancel)(LongRunning) = nullptr;

    constexpr Table(StrView name) : OnOff::Table(name, Interface::kLongRunning) {}
    template <typename ImplT>
    constexpr void FillFrom() {
      struct FullImpl : ImplT {
        bool IsOn() const { return this->IsRunning(); }
        void OnTurnOn() {
          if (auto r = this->object_ptr->template Find<Runnable>()) r.ScheduleRun(*this);
        }
        void OnTurnOff() { this->CancelWithoutNotify(); }
      };
      OnOff::Table::FillFrom<FullImpl>();
      if constexpr (requires(ImplT& i) { i.OnCancel(); })
        on_cancel = [](LongRunning self) { static_cast<ImplT&>(self).OnCancel(); };
    }
  };

  INTERFACE_BOUND(LongRunning, OnOff)

  bool IsRunning() const { return state->task != nullptr; }

  void BeginLongRunning(std::unique_ptr<RunTask>&& task) const {
    auto iface = Interface(task->source.GetUnsafe(), task->source_interface);
    state->task = std::move(task);
    // Avoids notifying other synchronized objects if it was started from another sync'ed object.
    if (iface != *this) {
      NotifyTurnedOn();
    }
  }

  // Cancels this object without notifying other synchronized objects.
  void CancelWithoutNotify() const;

  void Cancel() const;

  // Called from arbitrary thread by the object when it finishes execution.
  void Done() const;

  // ImplT must provide:
  //   static constexpr StrView kName = "..."sv;
  //   static constexpr int Offset();  // offsetof(Parent, def_member)
  //   Optionally: void OnCancel();
  template <typename ImplT>
  struct Def : State, Interface::DefBase {
    using Impl = ImplT;
    using Bound = LongRunning;

    static constexpr Table MakeTable() {
      Table t(ImplT::kName);
      t.FillFrom<ImplT>();
      return t;
    }

    inline constinit static Table tbl = MakeTable();

    ~Def() {
      if (task) {
        Bind().Cancel();
      }
      if (source || !gear_weak.IsExpired()) {
        Bind().Unsync();
      }
    }
  };
};

}  // namespace automat