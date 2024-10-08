// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <memory>
#include <string>
#include <unordered_set>

#include "animation.hh"
#include "connection.hh"
#include "error.hh"
#include "object.hh"
#include "run_button.hh"
#include "tasks.hh"
#include "text_field.hh"
#include "time.hh"
#include "widget.hh"

namespace automat::gui {
struct ConnectionWidget;
}  // namespace automat::gui

namespace automat {

struct LongRunning;

struct ObjectAnimationState {
  animation::SpringV2<float> scale;
  animation::SpringV2<Vec2> position;
  animation::Approach<> transparency;
  animation::Approach<> highlight;
  animation::SpringV2<float> elevation;

  ObjectAnimationState();

  SkMatrix GetTransform(Vec2 scale_pivot) const;
  animation::Phase Tick(float delta_time, Vec2 target_position, float target_scale);
};

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
  constexpr static float kPositionSpringPeriod = 0.2s .count();
  constexpr static float kScaleSpringPeriod = 0.3s .count();
  constexpr static float kSpringHalfTime = kScaleSpringPeriod / 4;

  ObjectAnimationState& GetAnimationState(animation::Display&) const;

  // TODO: remove this and move it into TransformToChild
  SkMatrix GetTransform(animation::Display*) const;

  animation::PerDisplay<ObjectAnimationState> animation_state;

  Location* parent;

  std::unique_ptr<Object> object;

  // Name of this Location.
  std::string name;
  gui::RunButton run_button;

  Vec2 position = {0, 0};
  float scale = 1.f;

  // Connections of this Location.
  // Connection is owned by both incoming & outgoing locations.
  std::unordered_multiset<Connection*, ConnectionHasher, ConnectionEqual> outgoing;
  std::unordered_multiset<Connection*, ConnectionHasher, ConnectionEqual> incoming;

  std::unordered_set<Location*> update_observers;
  std::unordered_set<Location*> observing_updates;

  std::unordered_set<Location*> error_observers;
  std::unordered_set<Location*> observing_errors;

  time::SteadyPoint last_finished;

  RunTask run_task;
  LongRunning* long_running = nullptr;

  Location(Location* parent = nullptr);
  ~Location();

  std::string_view Name() const override {
    if (name.empty()) {
      return "Location";
    } else {
      return name;
    }
  }

  std::unique_ptr<Object> InsertHere(std::unique_ptr<Object>&& object) {
    this->object.swap(object);
    this->object->Relocate(this);
    return object;
  }

  Object* Create(const Object& prototype) {
    object = prototype.Clone();
    object->Relocate(this);
    return object.get();
  }

  template <typename T>
  T* Create() {
    return dynamic_cast<T*>(Create(T::proto));
  }

  // Remove the objects held by this location.
  //
  // Some containers may not allow empty locations so this function may also
  // delete the location. Check the return value.
  Location* Clear() {
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

  Object* Follow();
  void Put(std::unique_ptr<Object> obj);
  std::unique_ptr<Object> Take();

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
  void ScheduleLocalUpdate(Location& updated);

  // Add this object to the task queue. Once it's turn comes, its `Run` method
  // will be executed.
  void ScheduleRun();

  // Execute this object's Errored function using the task queue.
  void ScheduleErrored(Location& errored);

  ////////////////////////////
  // Misc
  ////////////////////////////

  // This function should register a connection from this location to the
  // `other` so that subsequent calls to `Find` will return `other`.
  //
  // The function tries to be clever and marks the connection as `to_direct` if
  // the current object defines an argument with the same type as the `other`
  // object.
  //
  // This function should also notify the object with the `ConnectionAdded`
  // call.
  Connection* ConnectTo(Location& other, Argument&,
                        Connection::PointerBehavior pointer_behavior = Connection::kFollowPointers);

  // Immediately execute this object's Updated function.
  void Updated(Location& updated) { object->Updated(*this, updated); }

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

  void ObserveUpdates(Location& other) {
    other.update_observers.insert(this);
    observing_updates.insert(&other);
  }

  void StopObservingUpdates(Location& other) {
    other.update_observers.erase(this);
    observing_updates.erase(&other);
  }

  void ObserveErrors(Location& other) {
    other.error_observers.insert(this);
    observing_errors.insert(&other);
  }

  std::string GetText() {
    auto* follow = Follow();
    if (follow == nullptr) {
      return "";
    }
    return follow->GetText();
  }
  double GetNumber() { return std::stod(GetText()); }

  // Immediately execute this object's Run function.
  void Run();

  // Immediately execute this object's Errored function.
  void Errored(Location& errored) { object->Errored(*this, errored); }

  Location* Rename(std::string_view new_name) {
    name = new_name;
    return this;
  }

  template <typename T>
  T* ThisAs() {
    return dynamic_cast<T*>(object.get());
  }
  template <typename T>
  T* As() {
    return dynamic_cast<T*>(Follow());
  }
  template <typename T>
  T* ParentAs() const {
    return parent ? dynamic_cast<T*>(parent->object.get()) : nullptr;
  }

  void SetText(std::string_view text) {
    std::string current_text = GetText();
    if (current_text == text) {
      return;
    }
    Follow()->SetText(*this, text);
    ScheduleUpdate();
  }
  void SetNumber(double number);

  animation::Phase PreDraw(gui::DrawContext&) const override;
  animation::Phase Draw(gui::DrawContext&) const override;
  // Location has no texture attached so it cacn't be invalidated. It draws into its parent
  // texture instead. This override allows one to invalidate the parent texture.
  void InvalidateDrawCache() const override;
  void InvalidateConnectionWidgets() const;
  std::unique_ptr<Action> FindAction(gui::Pointer&, gui::ActionTrigger) override;
  SkPath Shape(animation::Display*) const override;
  SkPath FieldShape(Object&) const;

  // Call this when the position of this location changes to update the autoconnect arguments.
  void UpdateAutoconnectArgs();

  // DEPRECATED. Returns the position in parent machine's coordinates where the connections for this
  // argument should start.
  // TODO: replace with Argument::Start
  Vec2AndDir ArgStart(animation::Display*, Argument&);
  ControlFlow VisitChildren(gui::Visitor& visitor) override;
  maf::Optional<Rect> TextureBounds(animation::Display*) const override;

  ////////////////////////////
  // Error reporting
  ////////////////////////////

  // First error caught by this Location.
  std::unique_ptr<Error> error;

  // These functions are defined later because they use the Machine class which
  // needs to be defined first.
  //
  // TODO: rethink this API so that Machine-related functions don't pollute the
  // Location interface.
  bool HasError();
  Error* GetError();
  void ClearError();

  Error* ReportError(std::string_view message,
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
  void ReportMissing(std::string_view property);

  std::string ToStr() const;
};

// Try to guess how this location is displayed. Results in a `out_path` that starts with Window
// and ends with this Location and its Object. This is an interim function - ideally in each
// context where a Location is displayed, it should be clear what the path is.
void GuessPath(Location& location, gui::Path& out_path);

// See `GuessPath`.
gui::DisplayContext GuessDisplayContext(Location&, animation::Display&);

void PositionBelow(Location& origin, Location& below);
void AnimateGrowFrom(Location& source, Location& grown);

}  // namespace automat