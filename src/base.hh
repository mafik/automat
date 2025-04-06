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
#include "format.hh"
#include "location.hh"
#include "log.hh"
#include "pointer.hh"
#include "prototypes.hh"
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

struct LongRunning {
  // Called from Automat thread when user wants to cancel the execution.
  virtual void Cancel() = 0;

  // Called from arbitrary thread by the object when it finishes execution.
  //
  // After this call, the object is free to release the memory related to this LongRunning instance
  // because its not going to be used again.
  void Done(Location& here);
};

struct Runnable : public virtual SharedBase {
  // Derived classes should override this method to implement their behavior.
  //
  // If an object represents a long running process, it should mark itself by setting the
  // here.long_running pointer.
  virtual void OnRun(Location& here) = 0;

  // Kicks off the execution of the object.
  void Run(Location& here);
};

struct LiveObject : Object {
  WeakPtr<Location> here = {};

  void Relocate(Location* new_here) override;
  void ConnectionAdded(Location& here, Connection& connection) override {
    if (auto live_arg = dynamic_cast<LiveArgument*>(&connection.argument)) {
      live_arg->ConnectionAdded(here, connection);
    }
  }
};

template <typename T>
bool IsRunning(const T& object) {
  if (auto h = object.here.lock()) {
    return h->long_running;
  }
  return false;
}

// 2D Canvas holding objects & a spaghetti of connections.
struct Machine : LiveObject, gui::Widget, gui::DropTarget {
  Machine();
  string name = "";
  deque<Ptr<Location>> locations;
  vector<Location*> front;
  vector<Location*> children_with_errors;

  Ptr<Location> Extract(Location& location);

  Location& CreateEmpty(const string& name = "");

  Location& Create(const Object& prototype, const string& name = "") {
    auto& h = CreateEmpty(name);
    h.Create(prototype);
    return h;
  }

  // Create an instance of T and return its location.
  //
  // The new instance is created from a prototype instance found in `prototypes`.
  template <typename T>
  Location& Create(const string& name = "") {
    return Create(*prototypes->Find<T>(), name);
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
    auto m = MakePtr<Machine>();
    for (auto& my_it : locations) {
      auto& other_h = m->CreateEmpty(my_it->name);
      other_h.Create(*my_it->object);
    }
    return m;
  }

  void Draw(SkCanvas& canvas) const override { Widget::Draw(canvas); }
  void PreDraw(SkCanvas&) const override;
  gui::DropTarget* AsDropTarget() override { return this; }
  bool CanDrop(Location&) const override { return true; }
  void SnapPosition(Vec2& position, float& scale, Location&, Vec2* fixed_point) override;
  void DropLocation(Ptr<Location>&&) override;

  SkPath Shape() const override;

  void FillChildren(maf::Vec<Ptr<Widget>>& children) override;
  void Relocate(Location* parent) override;

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

      if (auto parent_location = here.parent_location.lock()) {
        parent_location->ScheduleErrored(here);
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
      if (auto h = here.lock()) {
        if (!h->HasError()) {
          if (auto parent = h->ParentAs<Machine>()) {
            parent->ClearChildError(*h);
          }
        }
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
      error_context.ReportError("Can't set text on null pointer");
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
