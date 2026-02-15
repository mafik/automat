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

// Interface for objects that can run long running jobs.
//
// Object destructors should call OnLongRunningDestruct(self) to ensure that the long
// running job is cancelled.
struct LongRunning : OnOff {
  static bool classof(const Interface* i) { return i->kind == Interface::kLongRunning; }

  // Function pointer for cancellation behavior.
  void (*on_cancel)(const LongRunning&, Object&) = nullptr;

  // Function pointer to access the per-Object task storage.
  std::unique_ptr<RunTask>& (*get_task)(Object&) = nullptr;

  LongRunning(StrView name);

  template <typename T>
  LongRunning(StrView name, SyncState& (*get)(T&),
              std::unique_ptr<RunTask>& (*get_task_fn)(T&),
              void (*on_cancel_fn)(const LongRunning&, T&) = nullptr)
      : LongRunning(name) {
    get_sync_state = reinterpret_cast<SyncState& (*)(Object&)>(get);
    get_task = reinterpret_cast<std::unique_ptr<RunTask>& (*)(Object&)>(get_task_fn);
    on_cancel = reinterpret_cast<void (*)(const LongRunning&, Object&)>(on_cancel_fn);
  }

  void OnLongRunningDestruct(Object& self) const {
    if (IsRunning(self)) {
      Cancel(self);
    }
  }

  // Call this to cancel the long running job.
  void Cancel(Object& self) const {
    auto& task = get_task(self);
    if (task == nullptr) {
      ERROR << "LongRunning::Cancel called without a long_running_task";
      return;
    }
    if (on_cancel) on_cancel(*this, self);
    task.reset();
    NotifyTurnedOff(self);
  }

  bool IsRunning(const Object& self) const {
    return get_task ? get_task(const_cast<Object&>(self)) != nullptr : false;
  }

  // Called from arbitrary thread by the object when it finishes execution.
  void Done(Object& self) const;

  void BeginLongRunning(Object& self, std::unique_ptr<RunTask>&& task) const {
    get_task(self) = std::move(task);
    NotifyTurnedOn(self);
  }
};

struct Runnable;

struct Runnable : Syncable {
  static bool classof(const Interface* i) { return i->kind == Interface::kRunnable; }

  // Function pointer for run behavior.
  void (*on_run)(const Runnable&, Object&, std::unique_ptr<RunTask>&) = nullptr;

  Runnable(StrView name);

  template <typename T>
  Runnable(StrView name, SyncState& (*get)(T&),
           void (*on_run_fn)(const Runnable&, T&, std::unique_ptr<RunTask>&))
      : Runnable(name) {
    get_sync_state = reinterpret_cast<SyncState& (*)(Object&)>(get);
    on_run = reinterpret_cast<void (*)(const Runnable&, Object&, std::unique_ptr<RunTask>&)>(
        on_run_fn);
  }

  // Call this to run this + all synchronized Runnables
  void Run(Object& self, std::unique_ptr<RunTask>& run_task) const {
    ForwardDo<Runnable>(self, [&](Object& o, const Runnable& iface) {
      if (iface.on_run) iface.on_run(iface, o, run_task);
    });
  }

  // Call this if this Runnable has been externally executed. It'll run all of the synchronized
  // Runnables.
  void NotifyRun(Object& self, std::unique_ptr<RunTask>& run_task) const {
    ForwardNotify<Runnable>(self, [&](Object& o, const Runnable& iface) {
      if (iface.on_run) iface.on_run(iface, o, run_task);
    });
  }

  // Enqueue this Runnable to be executed at the earliest opportunity.
  void ScheduleRun(Object& self) const {
    (new RunTask(self.AcquireWeakPtr(), this))->Schedule();
  }
};

struct RunOption : TextOption {
  WeakPtr<Object> weak;
  const Runnable* runnable;
  RunOption(WeakPtr<Object> object, const Runnable& runnable);
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
