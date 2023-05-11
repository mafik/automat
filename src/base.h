#pragma once

#include <algorithm>
#include <cassert>
#include <compare>
#include <deque>
#include <functional>
#include <memory>
#include <source_location>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <include/core/SkCanvas.h>
#include <include/core/SkPath.h>
#include <modules/skottie/include/Skottie.h>

#include "animation.h"
#include "channel.h"
#include "connection.h"
#include "format.h"
#include "location.h"
#include "log.h"
#include "run_button.h"
#include "tasks.h"
#include "text_field.h"

namespace automaton {

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

template <typename T> std::unique_ptr<Object> Create() {
  return T::proto.Clone();
}

struct Argument {
  enum Precondition {
    kOptional,
    kRequiresLocation,
    kRequiresObject,
    kRequiresConcreteType,
  };
  // Used for annotating arguments so that (future) UI can show them
  // differently.
  enum Quantity {
    kSingle,
    kMultiple,
  };

  std::string name;
  Precondition precondition;
  Quantity quantity;
  std::vector<std::function<void(Location *location, Object *object,
                                 std::string &error)>>
      requirements;

  Argument(string_view name, Precondition precondition,
           Quantity quantity = kSingle)
      : name(name), precondition(precondition), quantity(quantity) {}

  template <typename T> Argument &RequireInstanceOf() {
    requirements.emplace_back(
        [name = name](Location *location, Object *object, std::string &error) {
          if (dynamic_cast<T *>(object) == nullptr) {
            error = f("The %s argument must be an instance of %s.",
                      name.c_str(), typeid(T).name());
          }
        });
    return *this;
  }

  void CheckRequirements(Location &here, Location *location, Object *object,
                         std::string &error) {
    for (auto &requirement : requirements) {
      requirement(location, object, error);
      if (!error.empty()) {
        return;
      }
    }
  }

  struct LocationResult {
    bool ok = true;
    bool follow_pointers = true;
    Location *location = nullptr;
  };

  LocationResult GetLocation(Location &here,
                             std::source_location source_location =
                                 std::source_location::current()) const {
    LocationResult result;
    auto conn_it = here.outgoing.find(name);
    if (conn_it != here.outgoing.end()) { // explicit connection
      auto c = conn_it->second;
      result.location = &c->to;
      result.follow_pointers =
          c->pointer_behavior == Connection::kFollowPointers;
    } else { // otherwise, search for other locations in this machine
      result.location = reinterpret_cast<Location *>(
          here.Nearby([&](Location &other) -> void * {
            if (other.name == name) {
              return &other;
            }
            return nullptr;
          }));
    }
    if (result.location == nullptr && precondition >= kRequiresLocation) {
      here.ReportError(f("The %s argument of %s is not connected.",
                         name.c_str(), here.LoggableString().c_str()),
                       source_location);
      result.ok = false;
    }
    return result;
  }

  struct ObjectResult : LocationResult {
    Object *object = nullptr;
    ObjectResult(LocationResult location_result)
        : LocationResult(location_result) {}
  };

  ObjectResult GetObject(Location &here,
                         std::source_location source_location =
                             std::source_location::current()) const {
    ObjectResult result(GetLocation(here, source_location));
    if (result.location) {
      if (result.follow_pointers) {
        result.object = result.location->Follow();
      } else {
        result.object = result.location->object.get();
      }
      if (result.object == nullptr && precondition >= kRequiresObject) {
        here.ReportError(f("The %s argument of %s is empty.", name.c_str(),
                           here.LoggableString().c_str()),
                         source_location);
        result.ok = false;
      }
    }
    return result;
  }

  struct FinalLocationResult : ObjectResult {
    Location *final_location = nullptr;
    FinalLocationResult(ObjectResult object_result)
        : ObjectResult(object_result) {}
  };

  FinalLocationResult
  GetFinalLocation(Location &here, std::source_location source_location =
                                       std::source_location::current()) const;

  template <typename T> struct TypedResult : ObjectResult {
    T *typed = nullptr;
    TypedResult(ObjectResult object_result) : ObjectResult(object_result) {}
  };

  template <typename T>
  TypedResult<T> GetTyped(Location &here, std::source_location source_location =
                                              std::source_location::current()) {
    TypedResult<T> result(GetObject(here, source_location));
    if (result.object) {
      result.typed = dynamic_cast<T *>(result.object);
      if (result.typed == nullptr && precondition >= kRequiresConcreteType) {
        here.ReportError(f("The %s argument is not an instance of %s.",
                           name.c_str(), typeid(T).name()),
                         source_location);
        result.ok = false;
      }
    }
    return result;
  }

  // The Loop ends when `callback` returns a value that is convertible to
  // `true`.
  template <typename T>
  T LoopLocations(Location &here, function<T(Location &)> callback) {
    auto [begin, end] = here.outgoing.equal_range(name);
    for (auto it = begin; it != end; ++it) {
      if (auto ret = callback(it->second->to)) {
        return ret;
      }
    }
    return T();
  }

  // The Loop ends when `callback` returns a value that is convertible to
  // `true`.
  template <typename T>
  T LoopObjects(Location &here, function<T(Object &)> callback) {
    return LoopLocations<T>(here, [&](Location &h) {
      if (Object *o = h.Follow()) {
        return callback(*o);
      } else {
        return T();
      }
    });
  }
};

extern Argument then_arg;

struct LiveArgument : Argument {
  LiveArgument(string_view name, Precondition precondition)
      : Argument(name, precondition) {}

  template <typename T> LiveArgument &RequireInstanceOf() {
    Argument::RequireInstanceOf<T>();
    return *this;
  }
  void Detach(Location &here) {
    auto connections = here.outgoing.equal_range(name);
    for (auto it = connections.first; it != connections.second; ++it) {
      auto &connection = it->second;
      here.StopObservingUpdates(connection->to);
    }
    // If there were no connections, try to find nearby objects instead.
    if (connections.first == here.outgoing.end()) {
      here.Nearby([&](Location &other) {
        if (other.name == name) {
          here.StopObservingUpdates(other);
        }
        return nullptr;
      });
    }
  }
  void Attach(Location &here) {
    auto connections = here.outgoing.equal_range(name);
    for (auto it = connections.first; it != connections.second; ++it) {
      auto &connection = it->second;
      here.ObserveUpdates(connection->to);
    }
    // If there were no connections, try to find nearby objects instead.
    if (connections.first == here.outgoing.end()) {
      here.Nearby([&](Location &other) {
        if (other.name == name) {
          here.ObserveUpdates(other);
        }
        return nullptr;
      });
    }
  }
  void Relocate(Location *old_self, Location *new_self) {
    if (old_self) {
      Detach(*old_self);
    }
    if (new_self) {
      Attach(*new_self);
    }
  }
  void ConnectionAdded(Location &here, string_view label,
                       Connection &connection) {
    // TODO: handle the case where `here` is observing nearby objects (without
    // connections) and a connection is added.
    // TODO: handle ConnectionRemoved
    if (label == name) {
      here.ObserveUpdates(connection.to);
      here.ScheduleLocalUpdate(connection.to);
    }
  }
  void Rename(Location &here, string_view new_name) {
    Detach(here);
    name = new_name;
    Attach(here);
  }
};

struct LiveObject : Object {
  Location *here = nullptr;
  virtual void Args(std::function<void(LiveArgument &)> cb) = 0;

  Widget *ParentWidget() override { return here; }
  void Relocate(Location *new_self) override {
    Args([old_self = here, new_self](LiveArgument &arg) {
      arg.Relocate(old_self, new_self);
    });
    here = new_self;
  }
  void ConnectionAdded(Location &here, string_view label,
                       Connection &connection) override {
    Args([&here, &label, &connection](LiveArgument &arg) {
      arg.ConnectionAdded(here, label, connection);
    });
  }
};

// 2D Canvas holding objects & a spaghetti of connections.
struct Machine : LiveObject {
  static const Machine proto;
  Machine() = default;
  string name = "";
  unordered_set<unique_ptr<Location>> locations;
  vector<Location *> front;
  vector<Location *> children_with_errors;

  Location &CreateEmpty(const string &name = "") {
    auto [it, already_present] = locations.emplace(new Location(here));
    Location *h = it->get();
    h->name = name;
    return *h;
  }

  Location &Create(const Object &prototype, const string &name = "") {
    auto &h = CreateEmpty(name);
    h.Create(prototype);
    return h;
  }

  // Create an instance of T and return its location.
  //
  // The new instance is created from a prototype instance in `T::proto`.
  template <typename T> Location &Create(const string &name = "") {
    return Create(T::proto, name);
  }

  string_view Name() const override { return name; }
  std::unique_ptr<Object> Clone() const override {
    Machine *m = new Machine();
    for (auto &my_it : locations) {
      auto &other_h = m->CreateEmpty(my_it->name);
      other_h.Create(*my_it->object);
    }
    return std::unique_ptr<Object>(m);
  }

  SkPath Shape() const override {
    static SkPath empty_path;
    return empty_path;
  }
  gui::VisitResult
  VisitImmediateChildren(gui::WidgetVisitor &visitor) override {
    for (auto &it : locations) {
      SkMatrix transform_down =
          SkMatrix::Translate(-it->position.X, -it->position.Y);
      SkMatrix transform_up =
          SkMatrix::Translate(it->position.X, it->position.Y);
      auto result = visitor(*it, transform_down, transform_up);
      if (result == gui::VisitResult::kStop) {
        return result;
      }
    }
    return gui::VisitResult::kContinue;
  }
  void Args(std::function<void(LiveArgument &)> cb) override {}
  void Relocate(Location *parent) override {
    for (auto &it : locations) {
      it->parent = here;
    }
    LiveObject::Relocate(parent);
  }

  string LoggableString() const { return f("Machine(%s)", name.c_str()); }

  Location *Front(const string &name) {
    for (int i = 0; i < front.size(); ++i) {
      if (front[i]->name == name) {
        return front[i];
      }
    }
    return nullptr;
  }

  Location *operator[](const string &name) {
    auto h = Front(name);
    if (h == nullptr) {
      ERROR() << "Component \"" << name << "\" of " << this->name
              << " is null!";
    }
    return h;
  }

  void AddToFrontPanel(Location &h) {
    if (std::find(front.begin(), front.end(), &h) == front.end()) {
      front.push_back(&h);
    } else {
      ERROR() << "Attempted to add already present " << h << " to " << *this
              << " front panel";
    }
  }

  // Report all errors that occured within this machine.
  //
  // This function will return all errors held by locations of this machine &
  // recurse into submachines.
  void Diagnostics(function<void(Location *, Error &)> error_callback) {
    for (auto &location : locations) {
      if (location->error) {
        error_callback(location.get(), *location->error);
      }
      if (auto submachine = dynamic_cast<Machine *>(location->object.get())) {
        submachine->Diagnostics(error_callback);
      }
    }
  }

  void Errored(Location &here, Location &errored) override {
    // If the error hasn't been cleared by other Errored calls, then propagate
    // it to the parent.
    if (errored.HasError()) {
      children_with_errors.push_back(&errored);
      for (Location *observer : here.error_observers) {
        observer->ScheduleErrored(errored);
      }

      if (here.parent) {
        here.parent->ScheduleErrored(here);
      } else {
        Error *error = errored.GetError();
        ERROR(error->source_location) << error->text;
      }
    }
  }

  void ClearChildError(Location &child) {
    if (auto it = std::find(children_with_errors.begin(),
                            children_with_errors.end(), &child);
        it != children_with_errors.end()) {
      children_with_errors.erase(it);
      if (!here->HasError()) {
        if (auto parent = here->ParentAs<Machine>()) {
          parent->ClearChildError(*here);
        }
      }
    }
  }

  void DrawContents(SkCanvas &canvas, animation::State &animation_state);
};

struct Pointer : LiveObject {
  virtual Object *Next(Location &error_context) const = 0;
  virtual void PutNext(Location &error_context,
                       std::unique_ptr<Object> obj) = 0;
  virtual std::unique_ptr<Object> TakeNext(Location &error_context) = 0;

  std::pair<Pointer &, Object *> FollowPointers(Location &error_context) const {
    const Pointer *ptr = this;
    Object *next = Next(error_context);
    while (next != nullptr) {
      if (Pointer *next_ptr = next->AsPointer()) {
        ptr = next_ptr;
        next = next_ptr->Next(error_context);
      } else {
        break;
      }
    }
    return {*const_cast<Pointer *>(ptr), next};
  }
  Object *Follow(Location &error_context) const {
    return FollowPointers(error_context).second;
  }
  void Put(Location &error_context, std::unique_ptr<Object> obj) {
    FollowPointers(error_context).first.PutNext(error_context, std::move(obj));
  }
  std::unique_ptr<Object> Take(Location &error_context) {
    return FollowPointers(error_context).first.TakeNext(error_context);
  }

  Pointer *AsPointer() override { return this; }
  string GetText() const override {
    if (auto *obj = Follow(*here)) {
      return obj->GetText();
    } else {
      return "null";
    }
  }
  void SetText(Location &error_context, string_view text) override {
    if (auto *obj = Follow(error_context)) {
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

extern std::deque<Task *> queue;
extern std::unordered_set<Location *> no_scheduling;
extern vector<Task *> global_successors;

bool NoScheduling(Location *location);

struct ThenGuard {
  std::vector<Task *> successors;
  std::vector<Task *> old_global_successors;
  ThenGuard(std::vector<Task *> &&successors)
      : successors(std::move(successors)) {
    old_global_successors = global_successors;
    global_successors = this->successors;
  }
  ~ThenGuard() {
    assert(global_successors == successors);
    global_successors = old_global_successors;
    for (Task *successor : successors) {
      auto &pred = successor->predecessors;
      if (pred.empty()) {
        successor->Schedule();
      }
    }
  }
};

struct NoSchedulingGuard {
  Location &location;
  NoSchedulingGuard(Location &location) : location(location) {
    no_scheduling.insert(&location);
  }
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

extern channel events;

void RunThread();

} // namespace automaton
