#pragma once
// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT

#include <include/core/SkCanvas.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkPath.h>
#include <modules/skottie/include/Skottie.h>

#include <algorithm>
#include <cassert>
#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "argument.hpp"
#include "deserializer.hpp"
#include "drag_action.hpp"
#include "error.hpp"
#include "format.hpp"
#include "location.hpp"
#include "log.hpp"
#include "on_off.hpp"
#include "pointer.hpp"
#include "prototypes.hpp"
#include "ptr.hpp"
#include "run_button.hpp"
#include "sync.hpp"
#include "tasks.hpp"
#include "widget.hpp"

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

// A deliverable impulse: something an Object exposes so that control flow
// (run buttons, Next chains, Timers) can poke it. Delivery runs the handler
// on a worker thread through a RunTask.
struct Signal : Interface {
  // What to do with a signal that arrives while the object's LongRunning is
  // active. An object's starting signal is inhibited (a running thing is not
  // started twice); a signal that performs one bounded unit of work (pull one
  // buffer, decode one frame) is delivered.
  enum WhileLongRunning : bool { kDeliver, kInhibit };

  struct Table : Interface::Table {
    static bool classof(const Interface::Table* i) { return i->kind == Interface::kSignal; }

    void (*on_run)(Signal, std::unique_ptr<RunTask>&) = nullptr;

    WhileLongRunning while_long_running;

    constexpr Table(StrView name, WhileLongRunning while_long_running = kDeliver)
        : Interface::Table(Interface::kSignal, name), while_long_running(while_long_running) {}

    template <typename ImplT>
    constexpr void FillFrom() {
      Interface::Table::FillFrom<ImplT>();
      on_run = [](Signal self, std::unique_ptr<RunTask>& t) { static_cast<ImplT&>(self).OnRun(t); };
    }
  };

  struct State {};

  INTERFACE_BOUND(Signal, Interface)

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
    using Bound = Signal;

    static constexpr Table MakeTable() {
      Table t(ImplT::kName);
      t.FillFrom<ImplT>();
      return t;
    }

    inline constinit static Table tbl = MakeTable();

    ~Def() {}
  };
};

// The signal that starts an Object's work. While the object's LongRunning is
// active the work is already happening, so this signal is inhibited.
struct Runnable : Signal {
  struct Table : Signal::Table {
    static bool classof(const Interface::Table* i) {
      return Signal::Table::classof(i) &&
             static_cast<const Signal::Table*>(i)->while_long_running == kInhibit;
    }

    constexpr Table(StrView name) : Signal::Table(name, kInhibit) {}
  };

  INTERFACE_BOUND(Runnable, Signal)

  template <typename ImplT>
  struct Def : Interface::DefBase {
    using Impl = ImplT;
    using Bound = Runnable;

    static constexpr Table MakeTable() {
      Table t(ImplT::kName);
      t.FillFrom<ImplT>();
      return t;
    }

    inline constinit static Table tbl = MakeTable();

    ~Def() {}
  };
};

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
          if (auto r = this->object_ptr->template As<Runnable>()) r.ScheduleRun(*this);
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

using NextArg = InterfaceArgument<Signal, Interface::kNextArg>;

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

#include "board.hpp"
