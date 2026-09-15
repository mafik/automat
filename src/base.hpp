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

// The command that starts an Object's work. While the object's LongRunning is
// active the work is already happening, so this command is inhibited.
struct Runnable : Command {
  struct Table : Command::Table {
    static bool classof(const Interface::Table* i) {
      return Command::Table::classof(i) &&
             static_cast<const Command::Table*>(i)->while_long_running == kInhibit;
    }

    constexpr Table(StrView name) : Command::Table(name, kInhibit) {}
  };

  INTERFACE_BOUND(Runnable, Command)

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

using NextArg = InterfaceArgument<Command, Interface::kNextArg>;

extern Command::Table kThisIsFine;

struct Scalar : Interface {
  struct Table : Interface::Table {
    static bool classof(const Interface::Table* i) { return i->kind == Interface::kScalar; }

    double (*get)(Scalar) = nullptr;
    void (*set)(Scalar, double) = nullptr;

    constexpr Table(StrView name) : Interface::Table(Interface::kScalar, name) {}

    template <typename ImplT>
    constexpr void FillFrom() {
      Interface::Table::FillFrom<ImplT>();
      get = [](Scalar self) { return static_cast<ImplT&>(self).OnGet(); };
      set = [](Scalar self, double value) { static_cast<ImplT&>(self).OnSet(value); };
    }
  };

  struct State {};

  INTERFACE_BOUND(Scalar, Interface)

  double Get() const { return table->get(*this); }
  void Set(double value) const { table->set(*this, value); }

  template <typename ImplT>
  struct Def : Interface::DefBase {
    using Impl = ImplT;
    using Bound = Scalar;

    static constexpr Table MakeTable() {
      Table t(ImplT::kName);
      t.FillFrom<ImplT>();
      return t;
    }

    inline constinit static Table tbl = MakeTable();

    ~Def() {}
  };
};

struct Text : Interface {
  struct Table : Interface::Table {
    static bool classof(const Interface::Table* i) { return i->kind == Interface::kText; }

    Str (*get)(Text) = nullptr;
    void (*set)(Text, StrView) = nullptr;

    static std::unique_ptr<Action> DefaultActivate(Interface, ui::Pointer&, Toy*);

    constexpr Table(StrView name) : Interface::Table(Interface::kText, name) {
      cursor = ui::Cursor::IBeam;
      activate = &DefaultActivate;
    }

    template <typename ImplT>
    constexpr void FillFrom() {
      Interface::Table::FillFrom<ImplT>();
      get = [](Text self) { return static_cast<ImplT&>(self).OnGet(); };
      set = [](Text self, StrView value) { static_cast<ImplT&>(self).OnSet(value); };
    }
  };

  struct State {};

  INTERFACE_BOUND(Text, Interface)

  Str Get() const { return table->get(*this); }
  void Set(StrView value) const { table->set(*this, value); }

  template <typename ImplT>
  struct Def : Interface::DefBase {
    using Impl = ImplT;
    using Bound = Text;

    static constexpr Table MakeTable() {
      Table t(ImplT::kName);
      t.FillFrom<ImplT>();
      return t;
    }

    inline constinit static Table tbl = MakeTable();

    ~Def() {}
  };
};

// Interface for objects that can hold other objects within.
struct Container {
  // Remove the given `descendant` from this object and return it wrapped in a (possibly newly
  // created) Location.
  virtual Ptr<Location> Extract(Object& descendant) = 0;
};

}  // namespace automat

#include "location.hpp"
