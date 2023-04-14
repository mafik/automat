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
#include "format.h"
#include "gui_button.h"
#include "log.h"
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

struct Connection {
  enum PointerBehavior { kFollowPointers, kTerminateHere };
  Location &from, &to;
  PointerBehavior pointer_behavior;
  Connection(Location &from, Location &to, PointerBehavior pointer_behavior)
      : from(from), to(to), pointer_behavior(pointer_behavior) {}
};

struct Pointer;

// Objects are interactive pieces of data & behavior.
//
// Instances of this class provide custom logic & appearance.
struct Object : gui::Widget {

  // Create a copy of this object.
  //
  // Subclasses of Object should have a static `proto` field, holding their own
  // prototype instance. This prototype will be cloned when new objects are
  // created. See the `Machine::Create` function for details.
  virtual std::unique_ptr<Object> Clone() const = 0;

  virtual void Relocate(Location *new_self) {}

  // Release the memory occupied by this object.
  virtual ~Object() = default;

  virtual string GetText() const { return ""; }
  virtual void SetText(Location &error_context, string_view text) {}
  // virtual void SetText(Location &error_context, string_view text) {
  //   auto error_message = std::format("{} doesn't support text input.",
  //   Name()); error_context.ReportError(error_message);
  // }

  // Pointer-like objects can be followed.
  virtual Pointer *AsPointer() { return nullptr; }

  virtual void ConnectionAdded(Location &here, string_view label,
                               Connection &connection) {}
  virtual void Run(Location &here) {}
  virtual void Updated(Location &here, Location &updated) { Run(here); }
  virtual void Errored(Location &here, Location &errored) {}
  virtual std::partial_ordering
  operator<=>(const Object &other) const noexcept {
    return GetText() <=> other.GetText();
  }
  void Draw(SkCanvas &canvas, animation::State &animation_state) const override;
  SkPath Shape() const override;
};

std::vector<const Object *> &Prototypes();
void RegisterPrototype(const Object &prototype);

/*

The goal of Errors is to explain to the user what went wrong and help with
recovery.

Errors can be placed in Locations (alongside Objects). Each location can hold up
to one Error.

While present, Errors pause the execution at their locations. Each object is
responsible for checking the error at its location and taking it into account
when executing itself.

Errors keep track of their source (object? location?) which is usually the same
as their location. Some objects can trigger errors at remote locations to pause
them.

Errors can be cleaned by the user or by their source. The source of the error
should clean it automatically - but sometimes it can be executed explicitly to
recheck conditions & clean the error. Errors caused by failing preconditions
clear themselves automatically when an object is executed.

Errors can also save objects that would otherwise be deleted. The objects are
held in the Error instance and can be accessed by the user.

In the UI the errors are visualized as spiders sitting on the error locations.
Source of the error is indicated by a spider web. Saved objects are cocoons.

When an error is added to an object it causes a notification to be sent to all
`error_observers` of the object. The observers may fix the error or notify the
user somehow. The parent Machine is an implicit error observer and propagates
the error upwards. Top-level Machines print their errors to the console.

TODO: new "Error Eater" object - deletes any errors as soon as they're reported

*/

struct Error {
  std::string text;
  Location *source = nullptr;
  std::unique_ptr<Object> saved_object;
  std::source_location source_location;

  Error(std::string_view text,
        std::source_location source_location = std::source_location::current())
      : text(text), source_location(source_location) {}
};

inline std::ostream &operator<<(std::ostream &os, const Error &e) {
  return os << e.text;
}

struct string_equal {
  using is_transparent = std::true_type;

  bool operator()(string_view l, string_view r) const noexcept {
    return l == r;
  }
};

struct string_hash {
  using is_transparent = std::true_type;

  auto operator()(string_view str) const noexcept {
    return hash<string_view>()(str);
  }
};

template <typename Value>
using string_multimap =
    unordered_multimap<string, Value, string_hash, string_equal>;

template <typename T> struct ptr_hash {
  using is_transparent = void;

  auto operator()(T *p) const { return hash<T *>{}(p); }
  auto operator()(const unique_ptr<T> &p) const { return hash<T *>{}(p.get()); }
};

template <typename T> struct ptr_equal {
  using is_transparent = void;

  template <typename LHS, typename RHS>
  auto operator()(const LHS &lhs, const RHS &rhs) const {
    return AsPtr(lhs) == AsPtr(rhs);
  }

private:
  static const T *AsPtr(const T *p) { return p; }
  static const T *AsPtr(const unique_ptr<T> &p) { return p.get(); }
};

template <typename T> std::unique_ptr<Object> Create() {
  return T::proto.Clone();
}

// Each Container holds its inner objects in Locations.
//
// Location specifies location & relations of an object.
//
// Locations provide common interface for working with Containers of various
// types (2d canvas, 3d space, list, hashmap, etc.). In that sense they are
// similar to C++ iterators.
//
// Implementations of this interface would typically extend it with
// container-specific functions.
struct Location : gui::Widget {
  Location *parent;

  unique_ptr<Object> object;

  // Name of this Location.
  string name;
  gui::TextField name_text_field;
  gui::Button run_button;

  vec2 position = {0, 0};

  // Connections of this Location.
  // Connection is owned by both incoming & outgoing locations.
  string_multimap<Connection *> outgoing;
  string_multimap<Connection *> incoming;

  unordered_set<Location *> update_observers;
  unordered_set<Location *> observing_updates;

  unordered_set<Location *> error_observers;
  unordered_set<Location *> observing_errors;

  Location(Location *parent = nullptr);

  Widget *ParentWidget() override {
    if (parent == nullptr) {
      return nullptr;
    }
    return parent->object.get();
  }

  std::string_view Name() const override { return name; }

  std::unique_ptr<Object> InsertHere(unique_ptr<Object> &&object) {
    this->object.swap(object);
    return object;
  }

  Object *Create(const Object &prototype) {
    object = prototype.Clone();
    object->Relocate(this);
    return object.get();
  }

  template <typename T> T *Create() {
    return dynamic_cast<T *>(Create(T::proto));
  }

  // Remove the objects held by this location.
  //
  // Some containers may not allow empty locations so this function may also
  // delete the location. Check the return value.
  Location *Clear() {
    object.reset();
    return this;
  }

  ////////////////////////////
  // Pointer-like interface. This forwards the calls to the relevant Pointer
  // functions (if Pointer object is present). Otherwise operates directly on
  // this location.
  //
  // They are defined later because they need the Pointer class to be defined.
  //
  // TODO: rethink this API so that Pointer-related functions don't pollute the
  // Location interface.
  ////////////////////////////

  Object *Follow();
  void Put(unique_ptr<Object> obj);
  unique_ptr<Object> Take();

  ////////////////////////////
  // Task queue interface.
  //
  // These functions are defined later because they need Task classes to be
  // defined.
  //
  // TODO: rethink this API so that Task-related functions don't pollute the
  // Location interface.
  ////////////////////////////

  // Schedule this object's Updated function to be executed with the `updated`
  // argument.
  void ScheduleLocalUpdate(Location &updated);

  // Add this object to the task queue. Once it's turn comes, its `Run` method
  // will be executed.
  void ScheduleRun();

  // Execute this object's Errored function using the task queue.
  void ScheduleErrored(Location &errored);

  ////////////////////////////
  // Misc
  ////////////////////////////

  // Iterate over all nearby objects (including this object).
  //
  // Return non-null from the callback to stop the search.
  void *Nearby(function<void *(Location &)> callback);

  // This function should register a connection from this location to the
  // `other` so that subsequent calls to `Find` will return `other`.
  //
  // The function tries to be clever and marks the connection as `to_direct` if
  // the current object defines an argument with the same type as the `other`
  // object.
  //
  // This function should also notify the object with the `ConnectionAdded`
  // call.
  Connection *ConnectTo(Location &other, string_view label,
                        Connection::PointerBehavior pointer_behavior =
                            Connection::kFollowPointers);

  // Immediately execute this object's Updated function.
  void Updated(Location &updated) { object->Updated(*this, updated); }

  // Call this function when the value of the object has changed.
  //
  // It will notify all observers & call their `Updated` function.
  //
  // The `Updated` function will not be called immediately but will be scheduled
  // using the task queue.
  void ScheduleUpdate() {
    for (auto observer : update_observers) {
      observer->ScheduleLocalUpdate(*this);
    }
  }

  void ObserveUpdates(Location &other) {
    other.update_observers.insert(this);
    observing_updates.insert(&other);
  }

  void StopObservingUpdates(Location &other) {
    other.update_observers.erase(this);
    observing_updates.erase(&other);
  }

  void ObserveErrors(Location &other) {
    other.error_observers.insert(this);
    observing_errors.insert(&other);
  }

  string GetText() {
    auto *follow = Follow();
    if (follow == nullptr) {
      return "";
    }
    return follow->GetText();
  }
  double GetNumber() { return std::stod(GetText()); }

  // Immediately execute this object's Run function.
  void Run() { object->Run(*this); }

  // Immediately execute this object's Errored function.
  void Errored(Location &errored) { object->Errored(*this, errored); }

  Location *Rename(string_view new_name) {
    name = new_name;
    return this;
  }

  template <typename T> T *ThisAs() { return dynamic_cast<T *>(object.get()); }
  template <typename T> T *As() { return dynamic_cast<T *>(Follow()); }
  template <typename T> T *ParentAs() const {
    return parent ? dynamic_cast<T *>(parent->object.get()) : nullptr;
  }

  void SetText(string_view text) {
    std::string current_text = GetText();
    if (current_text == text) {
      return;
    }
    Follow()->SetText(*this, text);
    ScheduleUpdate();
  }
  void SetNumber(double number) { SetText(f("%lf", number)); }

  void Draw(SkCanvas &canvas, animation::State &animation_state) const override;
  SkPath Shape() const override;
  gui::VisitResult VisitImmediateChildren(gui::WidgetVisitor &visitor) override;

  ////////////////////////////
  // Error reporting
  ////////////////////////////

  // First error caught by this Location.
  unique_ptr<Error> error;

  // These functions are defined later because they use the Machine class which
  // needs to be defined first.
  //
  // TODO: rethink this API so that Machine-related functions don't pollute the
  // Location interface.
  bool HasError();
  Error *GetError();
  void ClearError();

  Error *
  ReportError(string_view message,
              std::source_location location = std::source_location::current()) {
    if (error == nullptr) {
      error.reset(new Error(message, location));
      error->source = this;
      for (auto observer : error_observers) {
        observer->ScheduleErrored(*this);
      }
      if (parent) {
        parent->ScheduleErrored(*this);
      }
    }
    return error.get();
  }

  // Shorthand function for reporting that a required property couldn't be
  // found.
  void ReportMissing(string_view property) {
    auto error_message =
        f("Couldn't find \"%*s\". You can create a connection or rename "
          "one of the nearby objects to fix this.",
          property.size(), property.data());
    ReportError(error_message);
  }

  string LoggableString() const {
    string_view object_name = object->Name();
    if (name.empty()) {
      if (object_name.empty()) {
        auto &o = *object;
        return typeid(o).name();
      } else {
        return std::string(object_name);
      }
    } else {
      return f("%*s \"%s\"", object_name.size(), object_name.data(),
               name.c_str());
    }
  }
};

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

extern std::deque<unique_ptr<Task>> queue;
extern std::unordered_set<Location *> no_scheduling;
extern std::shared_ptr<Task> *global_then;

bool NoScheduling(Location *location);

struct Task {
  Location *target;
  std::shared_ptr<Task> waiting_task;
  Task(Location *target) : target(target) {
    if (global_then) {
      waiting_task = *global_then;
    }
  }
  virtual ~Task() {}
  // Add this task to the task queue. This function takes ownership of the task.
  void Schedule() {
    if (NoScheduling(target)) {
      delete this;
      return;
    }
    if (log_executed_tasks) {
      LOG() << "Scheduling " << Format();
    }
    queue.emplace_back(this);
  }
  void PreExecute() {
    if (log_executed_tasks) {
      LOG() << Format();
      LOG_Indent();
    }
    if (waiting_task) {
      global_then = &waiting_task;
    }
  }
  void PostExecute() {
    if (global_then) {
      assert(global_then == &waiting_task);
      global_then = nullptr;
      if (waiting_task.use_count() == 1) {
        // std::shared_ptr doesn't permit moving data out of it so we have to
        // make a copy of the task for scheduling.
        waiting_task->Clone()->Schedule();
      }
    }
    if (log_executed_tasks) {
      LOG_Unindent();
    }
  }
  virtual std::string Format() { return "Task()"; }
  virtual void Execute() = 0;
  virtual Task *Clone() = 0;
};

struct RunTask : Task {
  RunTask(Location *target) : Task(target) {}
  std::string Format() override {
    return f("RunTask(%s)", target->LoggableString().c_str());
  }
  void Execute() override {
    PreExecute();
    target->Run();
    if (!target->HasError()) {
      then_arg.LoopLocations<bool>(*target, [](Location &then) {
        then.ScheduleRun();
        return false;
      });
    }
    PostExecute();
  }
  Task *Clone() override {
    auto t = new RunTask(target);
    t->waiting_task = waiting_task;
    return t;
  }
};

struct UpdateTask : Task {
  Location *updated;
  UpdateTask(Location *target, Location *updated)
      : Task(target), updated(updated) {}
  std::string Format() override {
    return f("UpdateTask(%s, %s)", target->LoggableString().c_str(),
             updated->LoggableString().c_str());
  }
  void Execute() override {
    PreExecute();
    target->Updated(*updated);
    PostExecute();
  }
  Task *Clone() override {
    auto t = new UpdateTask(target, updated);
    t->waiting_task = waiting_task;
    return t;
  }
};

struct FunctionTask : Task {
  std::function<void(Location &)> function;
  FunctionTask(Location *target, std::function<void(Location &)> function)
      : Task(target), function(function) {}
  std::string Format() override {
    return f("FunctionTask(%s)", target->LoggableString().c_str());
  }
  void Execute() override {
    PreExecute();
    function(*target);
    PostExecute();
  }
  Task *Clone() override {
    auto t = new FunctionTask(target, function);
    t->waiting_task = waiting_task;
    return t;
  }
};

struct ErroredTask : Task {
  Location *errored;
  ErroredTask(Location *target, Location *errored)
      : Task(target), errored(errored) {}
  std::string Format() override {
    return f("ErroredTask(%s, %s)", target->LoggableString().c_str(),
             errored->LoggableString().c_str());
  }
  void Execute() override {
    PreExecute();
    target->Errored(*errored);
    PostExecute();
  }
  Task *Clone() override {
    auto t = new ErroredTask(target, errored);
    t->waiting_task = waiting_task;
    return t;
  }
};

struct ThenGuard {
  std::shared_ptr<Task> then;
  std::shared_ptr<Task> *old_global_then;
  ThenGuard(std::unique_ptr<Task> &&then) : then(std::move(then)) {
    old_global_then = global_then;
    global_then = &this->then;
  }
  ~ThenGuard() {
    assert(global_then == &then);
    global_then = old_global_then;
    if (then.use_count() == 1) {
      then->Clone()->Schedule();
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
