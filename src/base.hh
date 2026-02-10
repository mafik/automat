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
// Destructors of derived classes should call OnLongRunningDestruct() to ensure that the long
// running job is cancelled. This must be done by the derived class because ~LongRunning shouldn't
// invoke abstract virtual functions (because derived class data has already been destroyed).
struct LongRunning : OnOff {
  std::unique_ptr<RunTask> long_running_task;

  virtual ~LongRunning() {
    if (IsRunning()) {
      ERROR << "Instance of the LongRunning interface didn't call OnLongRunningDestruct()";
    }
  }

  void OnLongRunningDestruct() {
    if (IsRunning()) {
      Cancel();
    }
  }

  // Implement this to implement cancellation of long running jobs.
  //
  // May be called from arbitrary thread.
  virtual void OnCancel() = 0;

  // Override this to find the object that's supposed to be executed
  virtual Object* OnFindRunnable() { return dynamic_cast<Object*>(this); }

  // Call this to cancel the long running job.
  void Cancel() {
    if (long_running_task == nullptr) {
      ERROR << "LongRunning::Cancel called without a long_running_task";
    }
    OnCancel();
    long_running_task.reset();
    NotifyTurnedOff();
  }

  bool IsRunning() const { return long_running_task != nullptr; }

  // Called from arbitrary thread by the object when it finishes execution.
  //
  // After this call, the object is free to release the memory related to this LongRunning instance
  // because its not going to be used again.
  void Done(Object& object);

  void BeginLongRunning(std::unique_ptr<RunTask>&& task) {
    long_running_task = std::move(task);
    NotifyTurnedOn();
  }

  bool IsOn() const override { return IsRunning(); }

 protected:
  void OnTurnOn() override;
  void OnTurnOff() override { Cancel(); }
};

struct Runnable;

struct SignalNext : virtual Atom {
  NestedWeakPtr<Runnable> next;
};

struct Runnable : Syncable, SignalNext {
  // Derived classes should override this method to implement their behavior.
  //
  // If an object must use the CPU for some computation it can stay busy as long as it needs to.
  // However if it's doing something in the background (like waiting for external resource) then it
  // should call BeginLongRunning with the run_task. Once it's done it should call Done.
  //
  // The RunTask is being passed here to make it possible to "steal" the task from the scheduler and
  // pass it to BeginLongRunning.
  virtual void OnRun(std::unique_ptr<RunTask>& run_task) = 0;

  // Call this to run this + all synchronized Runnables
  void Run(std::unique_ptr<RunTask>& run_task) {
    ForwardDo([&](Runnable& r) { r.OnRun(run_task); });
  }

  // Call this if this Runnable has been extrenally executed. It'll run all of the synchronized
  // Runnables.
  void NotifyRun(std::unique_ptr<RunTask>& run_task) {
    ForwardNotify([&](Runnable& r) { r.OnRun(run_task); });
  }

  // Enqueue this Runnable to be executed at the earliest opportunity. It may be executed on another
  // thread.
  //
  // Since this Runnable may be part of some Object (and that Object may be deleted while a Task
  // waits for execution), the scheduled Task will take a weak reference to the Object. If the
  // Object goes away, this Runnable will not be executed.
  void ScheduleRun(Object& self) { (new RunTask(self.AcquireWeakPtr(), this))->Schedule(); }

  bool CanSync(const Syncable& other) const override {
    return dynamic_cast<const Runnable*>(&other) != nullptr;
  }
};

struct RunOption : TextOption {
  WeakPtr<Object> weak;
  Runnable* runnable;
  RunOption(WeakPtr<Object> object, Runnable& runnable);
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

// Types of objects that sholud work nicely with data updates:
//
// - stateful functions (e.g. X + Y => Z)      Solution: function adds itself to
// the observers list, gets activated up by NotifyUpdated, recalculates its
// value & (maybe) calls NotifyUpdated on itself (or its output object)
// - bi-directional functions (X + 1 = Z)      Solution: same as above but the
// function activation must include direction (!)
// - lazy functions                            Solution: NotifyUpdated traverses
// all lazy nodes & activates their observers
//
// Complexity: O(connections + observers)

}  // namespace automat

#include "machine.hh"
