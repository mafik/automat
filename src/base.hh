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
#include <memory>
#include <source_location>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "animation.hh"
#include "argument.hh"
#include "channel.hh"
#include "connection.hh"
#include "control_flow.hh"
#include "deserializer.hh"
#include "drag_action.hh"
#include "format.hh"
#include "location.hh"
#include "log.hh"
#include "pointer.hh"
#include "prototypes.hh"
#include "run_button.hh"
#include "tasks.hh"
#include "text_field.hh"
#include "widget.hh"

namespace automat {

using std::deque;
using std::function;
using std::hash;
using std::string;
using std::string_view;
using std::unique_ptr;
using std::unordered_multimap;
using std::unordered_set;
using std::vector;

struct Error;
struct Object;
struct Location;
struct Machine;

struct LongRunning {
  // Called from Automat thread when user wants to cancel the execution.
  virtual void Cancel() = 0;

  // Called from arbitrary thread by the object when it finishes execution.
  //
  // After this call, the object is free to release the memory related to this LongRunning instance
  // because its not going to be used again.
  void Done(Location& here);
};

struct Runnable {
  // Derived classes should override this method to implement their behavior.
  //
  // If an object executes immediately, it should return nullptr. Otherwise it should return a
  // pointer to a LongRunning interface.
  virtual LongRunning* OnRun(Location& here) = 0;

  // Kicks off the execution of the object.
  void Run(Location& here);
};

struct LiveObject : Object {
  Location* here = nullptr;

  void Relocate(Location* new_self) override {
    Args([old_self = here, new_self](Argument& arg) {
      if (auto live_arg = dynamic_cast<LiveArgument*>(&arg)) {
        live_arg->Relocate(old_self, new_self);
      }
    });
    here = new_self;
  }
  void ConnectionAdded(Location& here, Connection& connection) override {
    if (auto live_arg = dynamic_cast<LiveArgument*>(&connection.argument)) {
      live_arg->ConnectionAdded(here, connection);
    }
  }
};

template <typename T>
bool IsRunning(const T& object) {
  return object.here && object.here->long_running;
}

// 2D Canvas holding objects & a spaghetti of connections.
struct Machine : LiveObject, gui::DropTarget {
  static const Machine proto;
  Machine();
  string name = "";
  deque<unique_ptr<Location>> locations;
  vector<Location*> front;
  vector<Location*> children_with_errors;

  std::unique_ptr<Location> Extract(Location& location);

  Location& CreateEmpty(const string& name = "") {
    auto& it = locations.emplace_front(new Location(here));
    Location* h = it.get();
    h->name = name;
    return *h;
  }

  Location& Create(const Object& prototype, const string& name = "") {
    auto& h = CreateEmpty(name);
    h.Create(prototype);
    return h;
  }

  // Create an instance of T and return its location.
  //
  // The new instance is created from a prototype instance in `T::proto`.
  template <typename T>
  Location& Create(const string& name = "") {
    return Create(T::proto, name);
  }

  void SerializeState(Serializer& writer, const char* key) const override;

  void DeserializeState(Location& l, Deserializer& d) override;

  Location* LocationAtPoint(Vec2);

  // Iterate over all nearby objects (within the given radius around start point).
  //
  // Return non-null to stop iteration and return from Nearby.
  void* Nearby(Vec2 center, float radius, std::function<void*(Location&)> callback);

  string_view Name() const override { return name; }
  std::unique_ptr<Object> Clone() const override {
    Machine* m = new Machine();
    for (auto& my_it : locations) {
      auto& other_h = m->CreateEmpty(my_it->name);
      other_h.Create(*my_it->object);
    }
    return std::unique_ptr<Object>(m);
  }

  animation::Phase Draw(gui::DrawContext& ctx) const override { return Widget::Draw(ctx); }
  animation::Phase PreDraw(gui::DrawContext&) const override;
  gui::DropTarget* CanDrop() override { return this; }
  void SnapPosition(Vec2& position, float& scale, Object* object, Vec2* fixed_point) override;
  void DropLocation(std::unique_ptr<Location>&&) override;

  SkPath Shape(animation::Display*) const override;

  ControlFlow VisitChildren(gui::Visitor& visitor) override;
  SkMatrix TransformToChild(const Widget& child, animation::Display*) const override;
  void Args(std::function<void(Argument&)> cb) override {}
  void Relocate(Location* parent) override {
    LiveObject::Relocate(parent);
    for (auto& it : locations) {
      it->parent = here;
    }
  }

  string ToStr() const { return maf::f("Machine(%s)", name.c_str()); }

  Location* Front(const string& name) {
    for (int i = 0; i < front.size(); ++i) {
      if (front[i]->name == name) {
        return front[i];
      }
    }
    return nullptr;
  }

  Location* operator[](const string& name) {
    auto h = Front(name);
    if (h == nullptr) {
      ERROR << "Component \"" << name << "\" of " << this->name << " is null!";
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
      if (location->error) {
        error_callback(location.get(), *location->error);
      }
      if (auto submachine = dynamic_cast<Machine*>(location->object.get())) {
        submachine->Diagnostics(error_callback);
      }
    }
  }

  void Errored(Location& here, Location& errored) override {
    // If the error hasn't been cleared by other Errored calls, then propagate
    // it to the parent.
    if (errored.HasError()) {
      children_with_errors.push_back(&errored);
      for (Location* observer : here.error_observers) {
        observer->ScheduleErrored(errored);
      }

      if (here.parent) {
        here.parent->ScheduleErrored(here);
      } else {
        Error* error = errored.GetError();
        maf::LogEntry(maf::LogLevel::Error, error->source_location) << error->text;
      }
    }
  }

  void ClearChildError(Location& child) {
    if (auto it = std::find(children_with_errors.begin(), children_with_errors.end(), &child);
        it != children_with_errors.end()) {
      children_with_errors.erase(it);
      if (!here->HasError()) {
        if (auto parent = here->ParentAs<Machine>()) {
          parent->ClearChildError(*here);
        }
      }
    }
  }
};

struct Pointer : LiveObject {
  virtual Object* Next(Location& error_context) const = 0;
  virtual void PutNext(Location& error_context, std::unique_ptr<Object> obj) = 0;
  virtual std::unique_ptr<Object> TakeNext(Location& error_context) = 0;

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
  void Put(Location& error_context, std::unique_ptr<Object> obj) {
    FollowPointers(error_context).first.PutNext(error_context, std::move(obj));
  }
  std::unique_ptr<Object> Take(Location& error_context) {
    return FollowPointers(error_context).first.TakeNext(error_context);
  }

  Pointer* AsPointer() override { return this; }
  string GetText() const override {
    if (auto* obj = Follow(*here)) {
      return obj->GetText();
    } else {
      return "null";
    }
  }
  void SetText(Location& error_context, string_view text) override {
    if (auto* obj = Follow(error_context)) {
      obj->SetText(error_context, text);
    } else {
      error_context.ReportError("Can't set text on null pointer");
    }
  }
};

extern int log_executed_tasks;

struct LogTasksGuard {
  LogTasksGuard();
  ~LogTasksGuard();
};

struct Task;

extern std::deque<Task*> queue;
extern std::unordered_set<Location*> no_scheduling;
extern vector<Task*> global_successors;

bool NoScheduling(Location* location);

struct NextGuard {
  std::vector<Task*> successors;
  std::vector<Task*> old_global_successors;
  NextGuard(std::vector<Task*>&& successors) : successors(std::move(successors)) {
    old_global_successors = global_successors;
    global_successors = this->successors;
  }
  ~NextGuard() {
    assert(global_successors == successors);
    global_successors = old_global_successors;
    for (Task* successor : successors) {
      auto& pred = successor->predecessors;
      if (pred.empty()) {
        successor->Schedule();
      }
    }
  }
};

// Sometimes objects are updated automatically (for example by their LiveArguments). This class
// allows such objects to block auto-scheduling and enable them to alter the values of their
// arguments without triggering re-runs.
struct NoSchedulingGuard {
  Location& location;
  NoSchedulingGuard(Location& location) : location(location) { no_scheduling.insert(&location); }
  ~NoSchedulingGuard() { no_scheduling.erase(&location); }
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

void RunLoop(const int max_iterations = -1);

// THIS IS THE MOST IMPORTANT OBJECT IN AUTOMAT - the only entry into the main loop.
extern channel events;

void RunThread(std::stop_token);

}  // namespace automat
