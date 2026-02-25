// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <include/core/SkCanvas.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkPath.h>
#include <modules/skottie/include/Skottie.h>

#include <algorithm>
#include <cassert>
#include <deque>
#include <functional>
#include <source_location>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "argument.hh"
#include "deserializer.hh"
#include "drag_action.hh"
#include "error.hh"
#include "format.hh"
#include "location.hh"
#include "log.hh"
#include "on_off.hh"
#include "pointer.hh"
#include "prototypes.hh"
#include "ptr.hh"
#include "run_button.hh"
#include "sync.hh"
#include "tasks.hh"
#include "widget.hh"

namespace automat {

using std::deque;
using std::function;
using std::hash;
using std::string;
using std::string_view;
using std::unordered_multimap;
using std::unordered_set;
using std::vector;

struct Error;
struct Object;
struct Location;

struct Runnable : Syncable {
  struct Table : Syncable::Table {
    static bool classof(const Interface::Table* i) { return i->kind == Interface::kRunnable; }

    void (*on_run)(Runnable, std::unique_ptr<RunTask>&) = nullptr;

    static bool DefaultCanSync(Syncable, Syncable other);

    constexpr Table(StrView name) : Syncable::Table(name, Interface::kRunnable) {
      can_sync = &DefaultCanSync;
    }

    template <typename ImplT>
    constexpr void FillFrom() {
      Syncable::Table::FillFrom<ImplT>();
      on_run = [](Runnable self, std::unique_ptr<RunTask>& t) {
        static_cast<ImplT&>(self).OnRun(t);
      };
    }
  };

  INTERFACE_BOUND(Runnable, Syncable)

  void Run(std::unique_ptr<RunTask>& run_task) const {
    ForwardDo([&](Runnable other) {
      if (other.table->on_run) other.table->on_run(other, run_task);
    });
  }

  void ScheduleRun() const { (new RunTask(object_ptr->AcquireWeakPtr(), table_ptr))->Schedule(); }

  void NotifyRun(std::unique_ptr<RunTask>& run_task) const {
    ForwardNotify([&](Runnable other) {
      if (other.table->on_run) other.table->on_run(other, run_task);
    });
  }

  // ImplT must provide:
  //   static constexpr StrView kName = "..."sv;
  //   static constexpr int Offset();  // offsetof(Parent, def_member)
  //   void OnRun(std::unique_ptr<RunTask>&);
  template <typename ImplT>
  struct Def : Syncable::State, Interface::DefBase {
    using Impl = ImplT;
    using Bound = Runnable;

    static constexpr Table MakeTable() {
      Table t(ImplT::kName);
      t.FillFrom<ImplT>();
      return t;
    }

    inline constinit static Table tbl = MakeTable();

    ~Def() {
      if (source || !end.IsExpired()) {
        Bind().Unsync();
      }
    }
  };
};

struct LongRunning : OnOff {
  struct State : OnOff::State {
    std::unique_ptr<RunTask> task;
  };

  struct Table : OnOff::Table {
    static bool classof(const Interface::Table* i) { return i->kind == Interface::kLongRunning; }

    void (*on_cancel)(LongRunning) = nullptr;

    static bool DefaultIsOn(OnOff self) {
      return LongRunning(*self.object_ptr, static_cast<Table&>(*self.table_ptr)).IsRunning();
    }
    static void DefaultOnTurnOn(OnOff self) {
      if (auto r = self.object_ptr->As<Runnable>()) r.ScheduleRun();
    }
    static void DefaultOnTurnOff(OnOff self) {
      LongRunning(*self.object_ptr, static_cast<Table&>(*self.table_ptr)).Cancel();
    }

    constexpr Table(StrView name) : OnOff::Table(name, Interface::kLongRunning) {
      is_on = &DefaultIsOn;
      on_turn_on = &DefaultOnTurnOn;
      on_turn_off = &DefaultOnTurnOff;
    }

    template <typename ImplT>
    constexpr void FillFrom() {
      struct FullImpl : ImplT {
        bool IsOn() const { return this->IsRunning(); }
        void OnTurnOn() {
          if (auto r = this->object_ptr->template As<Runnable>()) r.ScheduleRun();
        }
        void OnTurnOff() { this->Cancel(); }
      };
      OnOff::Table::FillFrom<FullImpl>();
      if constexpr (requires(ImplT& i) { i.OnCancel(); })
        on_cancel = [](LongRunning self) { static_cast<ImplT&>(self).OnCancel(); };
    }
  };

  INTERFACE_BOUND(LongRunning, OnOff)

  bool IsRunning() const { return state->task != nullptr; }

  void BeginLongRunning(std::unique_ptr<RunTask>&& task) const {
    state->task = std::move(task);
    NotifyTurnedOn();
  }

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
      if (source || !end.IsExpired()) {
        Bind().Unsync();
      }
    }
  };
};

using NextArg = InterfaceArgument<Runnable, Interface::kNextArg>;

struct RunOption : TextOption {
  WeakPtr<Object> weak;
  Runnable::Table* runnable;
  RunOption(WeakPtr<Object> object, Runnable::Table& runnable);
  std::unique_ptr<Option> Clone() const override;
  std::unique_ptr<Action> Activate(ui::Pointer& pointer) const override;
  Dir PreferredDir() const override { return S; }
};

// Interface for objects that can hold other objects within.
struct Container {
  // Remove the given `descendant` from this object and return it wrapped in a (possibly newly
  // created) Location.
  virtual Ptr<Location> Extract(Object& descendant) = 0;
};

}  // namespace automat

#include "board.hh"
