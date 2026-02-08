// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <memory>
#include <string>
#include <unordered_set>

#include "animation.hh"
#include "object.hh"
#include "run_button.hh"
#include "tasks.hh"
#include "text_field.hh"
#include "time.hh"
#include "toy.hh"
#include "widget.hh"

namespace automat::ui {
struct ConnectionWidget;
}  // namespace automat::ui

namespace automat {

struct LongRunning;
struct LocationWidget;

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
struct Location : ReferenceCounted, ToyMakerMixin {
  WeakPtr<Location> parent_location;
  using Toy = LocationWidget;

  Ptr<Object> object;

  Vec2 position = {0, 0};
  mutable float scale = 1.f;

  std::unordered_set<Location*> update_observers;
  std::unordered_set<Location*> observing_updates;

  // Cached LocationWidget (set by MakeToy, cleared by LocationWidget dtor).
  LocationWidget* widget = nullptr;

  // ToyMaker concept
  ReferenceCounted& GetOwner() { return *this; }
  Atom& GetAtom() { return *this; }
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent);

  // Obtain a matrix representation of the given transform.
  static SkMatrix ToMatrix(Vec2 position, float scale, Vec2 anchor);
  // Get the position & scale out of the given matrix & anchor.
  static void FromMatrix(const SkMatrix& matrix, const Vec2& anchor, Vec2& out_position,
                         float& out_scale);

  void Iconify();
  void Deiconify();

  explicit Location(WeakPtr<Location> parent_location = {});
  ~Location();

  // Find (or create if needed) the Widget for this location's object.
  Object::Toy& ToyForObject();

  void InvalidateConnectionWidgets(bool moved, bool value_changed) const;

  Ptr<Object> InsertHere(Ptr<Object>&& object);

  Ptr<Object> Create(const Object& prototype);

  template <typename T, typename... Args>
  Ptr<T> Create(Args&&... args) {
    auto typed = MAKE_PTR(T, std::forward<Args>(args)...);
    object = typed;
    object->Relocate(this);
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

  ////////////////////////////
  // Misc
  ////////////////////////////

  // Immediately execute this object's Updated function.
  void Updated(WeakPtr<Object>& updated) { object->Updated(updated); }

  // Call this function when the value of the object has changed.
  //
  // It will notify all observers & call their `Updated` function.
  //
  // The `Updated` function will not be called immediately but will be scheduled
  // using the task queue.
  void ScheduleUpdate() {
    for (auto observer : update_observers) {
      (new UpdateTask(observer->object->AcquireWeakPtr(), object->AcquireWeakPtr()))->Schedule();
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

  std::string GetText() {
    auto* follow = Follow();
    if (follow == nullptr) {
      return "";
    }
    return follow->GetText();
  }
  double GetNumber() { return std::stod(GetText()); }

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
    Follow()->SetText(text);
    ScheduleUpdate();
  }
  void SetNumber(double number);
};

struct LocationWidget : Toy {
  constexpr static float kPositionSpringPeriod = 0.2;
  constexpr static float kScaleSpringPeriod = 0.3;
  constexpr static float kSpringHalfTime = kScaleSpringPeriod / 4;

  WeakPtr<Location> location_weak;

  // Animation state (moved from Location)
  float transparency = 0;
  animation::SpringV2<float> elevation;
  Optional<Vec2> local_anchor;
  Vec2 position_vel = {};
  float scale_vel = 0;
  Object::Toy* toy = nullptr;  // cached Object Toy
  std::vector<ui::Widget*> overlays;

  LocationWidget(ui::Widget* parent, Location& loc);
  ~LocationWidget();

  Ptr<Location> LockLocation() const { return location_weak.Lock(); }

  Object::Toy& ToyForObject();
  Vec2 LocalAnchor() const override;

  // Widget overrides
  animation::Phase Tick(time::Timer& timer) override;
  void PreDraw(SkCanvas&) const override;
  void Draw(SkCanvas&) const override;
  SkPath Shape() const override;
  SkPath ShapeRigid() const override;
  void FillChildren(Vec<ui::Widget*>& children) override;
  Optional<Rect> TextureBounds() const override;
  std::unique_ptr<Action> FindAction(ui::Pointer&, ui::ActionTrigger) override;

  // Call this when the position of this location changes to update the autoconnect arguments.
  void UpdateAutoconnectArgs();

  // DEPRECATED. Returns the position in parent's coordinates where the connections for this
  // argument should start.
  // TODO: replace with Toy::ArgStart
  Vec2AndDir ArgStart(Argument&);
};

static_assert(ToyMaker<Location>);

void PositionBelow(Location& origin, Location& below);

// Place the given `target` location ahead of the `origin`s `arg`.
//
// This uses the arg's position & direction within `origin`.
//
// This version just returns the recommended position for the target_widget.
Vec2 PositionAhead(Location& origin, const Argument& arg, const Object::Toy& target_widget);

// Similar to the above, but also sets the target's position.
void PositionAhead(Location& origin, const Argument& arg, Location& target);

void AnimateGrowFrom(Location& source, Location& grown);

}  // namespace automat
