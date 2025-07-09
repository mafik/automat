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
  float transparency = 0;
  float highlight = 0;
  float highlight_target = 0;
  time::T time_seconds = 0;  // used to animate dashed line
  animation::SpringV2<float> elevation;
  Optional<Vec2> scale_pivot;

  ObjectAnimationState();

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
struct Location : public gui::Widget {
  constexpr static float kPositionSpringPeriod = 0.2s .count();
  constexpr static float kScaleSpringPeriod = 0.3s .count();
  constexpr static float kSpringHalfTime = kScaleSpringPeriod / 4;

  ObjectAnimationState& GetAnimationState() const;
  void UpdateChildTransform();

  mutable ObjectAnimationState animation_state;

  WeakPtr<Location> parent_location;

  Ptr<Object> object;
  mutable Ptr<Widget> object_widget;

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

  // Retained to make it possible to check if the task is scheduled & cancel it.
  // Initialized lazily (may be nullptr).
  std::unique_ptr<RunTask> run_task;

  RunTask& GetRunTask();

  Location(WeakPtr<Location> parent = {});
  ~Location();

  // Find (or create if needed) the Widget for this location's object.
  // Shortcut for Widget::ForObject(location.object, location)
  Ptr<Widget>& WidgetForObject() const {
    if (!object_widget) {
      if (object) {
        object_widget = Widget::ForObject(*object, *this);
      }
    }
    return object_widget;
  }

  Vec2 ScalePivot() const override;

  // A version of InsertHere that doesn't create a Widget for the object.
  //
  // TODO: Remove InsertHere and switch to this one. Take care to find and fix all places which
  // depend on implicitly created widgets.
  Ptr<Object> InsertHereNoWidget(Ptr<Object>&& object);

  Ptr<Object> InsertHere(Ptr<Object>&& object);

  Ptr<Object> Create(const Object& prototype);

  template <typename T>
  Ptr<T> Create() {
    auto typed = MakePtr<T>();
    object = typed;
    object->Relocate(this);
    auto object_widget = WidgetForObject();
    object_widget->parent = this->AcquirePtr();
    FixParents();
    return typed;
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
  void Put(Ptr<Object> obj);
  Ptr<Object> Take();

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

  // Immediately execute this object's Errored function.
  void Errored(Location& errored) { object->Errored(*this, errored); }

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
    if (auto p = parent_location.lock()) {
      return dynamic_cast<T*>(p->object.get());
    }
    return nullptr;
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

  animation::Phase Tick(time::Timer& timer) override;
  void PreDraw(SkCanvas&) const override;
  void Draw(SkCanvas&) const override;
  void InvalidateConnectionWidgets(bool moved, bool value_changed) const;
  std::unique_ptr<Action> FindAction(gui::Pointer&, gui::ActionTrigger) override;
  SkPath Shape() const override;
  SkPath FieldShape(Object&) const override;

  // Call this when the position of this location changes to update the autoconnect arguments.
  //
  // IMPORTANT: this function uses the widget-based screen coordinates to determine the position
  // of the object. Pay attention to the parent location's animation_state!
  void UpdateAutoconnectArgs();

  // DEPRECATED. Returns the position in parent machine's coordinates where the connections for this
  // argument should start.
  // TODO: replace with Argument::Start
  Vec2AndDir ArgStart(Argument&);
  void FillChildren(Vec<Ptr<Widget>>& children) override;
  Optional<Rect> TextureBounds() const override;

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
      if (auto p = parent_location.lock()) {
        p->ScheduleErrored(*this);
      }
    }
    return error.get();
  }

  // Shorthand function for reporting that a required property couldn't be
  // found.
  void ReportMissing(std::string_view property);

  std::string ToStr() const;
};

void PositionBelow(Location& origin, Location& below);

// Place the given `target` location ahead of the `origin`s `arg`.
//
// This uses the arg's position & direction within `origin`.
void PositionAhead(Location& origin, Argument& arg, Location& target);

void AnimateGrowFrom(Location& source, Location& grown);

}  // namespace automat
