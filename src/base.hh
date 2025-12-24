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
#include "connection.hh"
#include "deserializer.hh"
#include "drag_action.hh"
#include "error.hh"
#include "format.hh"
#include "interfaces.hh"
#include "location.hh"
#include "log.hh"
#include "on_off.hh"
#include "pointer.hh"
#include "prototypes.hh"
#include "ptr.hh"
#include "run_button.hh"
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
struct Machine;

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

  // Call this to cancel the long running job.
  void Cancel() {
    if (long_running_task == nullptr) {
      FATAL << "LongRunning::Cancel called without a long_running_task";
    }
    OnCancel();
    long_running_task.reset();
  }

  bool IsRunning() const { return long_running_task != nullptr; }

  // Called from arbitrary thread by the object when it finishes execution.
  //
  // After this call, the object is free to release the memory related to this LongRunning instance
  // because its not going to be used again.
  void Done(Location& here);

  void BeginLongRunning(std::unique_ptr<RunTask>&& task) { long_running_task = std::move(task); }

  bool IsOn() const override { return IsRunning(); }

 protected:
  void OnTurnOn() override;
  void OnTurnOff() override { Cancel(); }
};

struct Runnable {
  // Derived classes should override this method to implement their behavior.
  //
  // If an object must use the CPU for some computation it can stay busy as long as it needs to.
  // However if it's doing something in the background (like waiting for external resource) then it
  // should call BeginLongRunning with the run_task. Once it's done it should call Done.
  //
  // The RunTask is being passed here to make it possible to "steal" the task from the scheduler and
  // pass it to BeginLongRunning.
  virtual void OnRun(Location& here, std::unique_ptr<RunTask>& run_task) = 0;
};

struct LiveObject : Object {
  WeakPtr<Location> here;

  void Relocate(Location* new_here) override;
  void ConnectionAdded(Location& here, Connection& connection) override {
    if (auto live_arg = dynamic_cast<LiveArgument*>(&connection.argument)) {
      live_arg->ConnectionAdded(here, connection);
    }
  }
  void ConnectionRemoved(Location& here, Connection& connection) override {
    if (auto live_arg = dynamic_cast<LiveArgument*>(&connection.argument)) {
      live_arg->ConnectionRemoved(here, connection);
    }
  }
};

// 2D Canvas holding objects & a spaghetti of connections.
struct Machine : LiveObject, ui::Widget, ui::DropTarget {
  Machine(ui::Widget* parent);
  string name = "";
  deque<Ptr<Location>> locations;
  vector<Location*> front;

  Ptr<Location> Extract(Location& location);
  Vec<Ptr<Location>> ExtractStack(Location& base);

  // Create a new location on top of all the others.
  Location& CreateEmpty();

  Location& Create(const Object& prototype) {
    auto& h = CreateEmpty();
    h.Create(prototype);
    return h;
  }

  // Create an instance of T and return its location.
  //
  // The new instance is created from a prototype instance found in `prototypes`.
  template <typename T>
  Location& Create() {
    return Create(*prototypes->Find<T>());
  }

  void SerializeState(Serializer& writer, const char* key) const override;

  void DeserializeState(Location& l, Deserializer& d) override;

  Location* LocationAtPoint(Vec2);

  // Iterate over all nearby objects (within the given radius around start point).
  //
  // Return non-null to stop iteration and return from Nearby.
  void* Nearby(Vec2 center, float radius, std::function<void*(Location&)> callback);

  string_view Name() const override { return name; }
  Ptr<Object> Clone() const override {
    auto m = MAKE_PTR(Machine, this->parent);
    for (auto& my_it : locations) {
      auto& other_h = m->CreateEmpty();
      other_h.Create(*my_it->object);
    }
    return m;
  }

  void Draw(SkCanvas& canvas) const override { Widget::Draw(canvas); }
  void PreDraw(SkCanvas&) const override;
  Compositor GetCompositor() const override { return Compositor::QUANTUM_REALM; }
  ui::DropTarget* AsDropTarget() override { return this; }
  bool CanDrop(Location&) const override { return true; }
  SkMatrix DropSnap(const Rect& bounds_local, Vec2 bounds_origin,
                    Vec2* fixed_point = nullptr) override;
  void DropLocation(Ptr<Location>&&) override;

  SkPath Shape() const override;

  void FillChildren(Vec<Widget*>& children) override;
  void Relocate(Location* parent) override;

  string ToStr() const { return f("Machine({})", name); }

  Location* Front(int i) {
    if (i < 0 || i >= front.size()) {
      return nullptr;
    }
    return front[i];
  }

  Location* operator[](int i) {
    auto h = Front(i);
    if (h == nullptr) {
      ERROR << "Component \"" << i << "\" of " << this->name << " is null!";
    }
    return h;
  }

  void AddToFrontPanel(Location& h) {
    if (std::find(front.begin(), front.end(), &h) == front.end()) {
      front.push_back(&h);
    } else {
      ERROR << "Attempted to add already present " << h << " to " << *this << " front panel";
    }
  }

  // Report all errors that occured within this machine.
  //
  // This function will return all errors held by locations of this machine &
  // recurse into submachines.
  void Diagnostics(function<void(Location*, Error&)> error_callback) {
    for (auto& location : locations) {
      ManipulateError(*location->object, [&](Error& err) {
        if (!err.IsPresent()) {
          return;
        }
        error_callback(location.get(), err);
      });
      if (auto submachine = dynamic_cast<Machine*>(location->object.get())) {
        submachine->Diagnostics(error_callback);
      }
    }
  }
};

struct Pointer : LiveObject {
  virtual Object* Next(Location& error_context) const = 0;
  virtual void PutNext(Location& error_context, Ptr<Object> obj) = 0;
  virtual Ptr<Object> TakeNext(Location& error_context) = 0;

  std::pair<Pointer&, Object*> FollowPointers(Location& error_context) const {
    const Pointer* ptr = this;
    Object* next = Next(error_context);
    while (next != nullptr) {
      if (Pointer* next_ptr = next->AsPointer()) {
        ptr = next_ptr;
        next = next_ptr->Next(error_context);
      } else {
        break;
      }
    }
    return {*const_cast<Pointer*>(ptr), next};
  }
  Object* Follow(Location& error_context) const { return FollowPointers(error_context).second; }
  void Put(Location& error_context, Ptr<Object> obj) {
    FollowPointers(error_context).first.PutNext(error_context, std::move(obj));
  }
  Ptr<Object> Take(Location& error_context) {
    return FollowPointers(error_context).first.TakeNext(error_context);
  }

  Pointer* AsPointer() override { return this; }
  string GetText() const override {
    if (auto h = here.lock()) {
      if (auto* obj = Follow(*h)) {
        return obj->GetText();
      }
    }
    return "null";
  }
  void SetText(Location& error_context, string_view text) override {
    if (auto* obj = Follow(error_context)) {
      obj->SetText(error_context, text);
    } else {
      error_context.object->ReportError("Can't set text on null pointer");
    }
  }
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
