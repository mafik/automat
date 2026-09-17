#pragma once
// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT

#include <memory>
#include <string>
#include <unordered_set>
#include <variant>

#include "animation.hpp"
#include "base.hpp"
#include "interface.hpp"
#include "object.hpp"
#include "object_source.hpp"
#include "ptr.hpp"
#include "tasks.hpp"
#include "time.hpp"
#include "toy.hpp"
#include "widget.hpp"

namespace automat::ui {
struct ConnectionWidget;
}  // namespace automat::ui

namespace automat {

struct LongRunning;
struct LocationWidget;
struct Board;

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
struct Location : Object {
  // The Board holding this Location (null while outside of a Board).
  WeakPtr<Board> board;

  using Toy = LocationWidget;

  Owned<Object> object{*this};

  struct Direct {
    Vec2 position = {0, 0};
    float scale = 1.f;
  };
  struct PlaceAhead {
    WeakPtr<Location> origin;
    Interface::Table* arg;
  };
  struct PlaceBetween {
    WeakPtr<Location> a, b;
  };
  struct PlaceBeside {
    WeakPtr<Location> origin;
  };
  std::variant<Direct, PlaceAhead, PlaceBetween, PlaceBeside> placement = Direct{};

  void FillPosition(LocationWidget& w);  // private

  Vec2& Position(LocationWidget& w) {
    if (!std::holds_alternative<Direct>(placement)) {
      FillPosition(w);
    }
    return std::get_if<Direct>(&placement)->position;
  }

  float& Scale(LocationWidget& w) {
    if (!std::holds_alternative<Direct>(placement)) {
      FillPosition(w);
    }
    return std::get_if<Direct>(&placement)->scale;
  }

  Vec2 PeekPosition() const {
    auto* direct = std::get_if<Direct>(&placement);
    return direct ? direct->position : Vec2{};
  }
  float PeekScale() const {
    auto* direct = std::get_if<Direct>(&placement);
    return direct ? direct->scale : 1.f;
  }

  std::unordered_set<Location*> update_observers;
  std::unordered_set<Location*> observing_updates;

  // DEPRECATED: an Engine-space object must not reference UI-space widgets. Widgets are not
  // thread-aware and may vanish between two Engine reads.
  MortalPtr<LocationWidget> widget;

  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;

  // Obtain a matrix representation of the given transform.
  static SkMatrix ToMatrix(Vec2 position, float scale, Vec2 anchor);
  // Get the position & scale out of the given matrix & anchor.
  static void FromMatrix(const SkMatrix& matrix, const Vec2& anchor, Vec2& out_position,
                         float& out_scale);

  // Controls iconification of the stored ObjectToy (not LocationWdiget!)
  bool iconified = false;

  void Iconify();
  void Deiconify();

  DEF_INTERFACE(Location, ObjectSource, move, "Move")
  Ptr<Object> OnTake();
  std::unique_ptr<Action> OnActivate(ui::Pointer&, automat::Toy*);
  DEF_END(move);

  DEF_INTERFACE(Location, ObjectSource, copy, "Copy")
  Ptr<Object> OnTake();
  std::unique_ptr<Action> OnActivate(ui::Pointer&, automat::Toy*);
  DEF_END(copy);

  DEF_INTERFACE(Location, ObjectSource, clone, "Clone")
  Ptr<Object> OnTake();
  std::unique_ptr<Action> OnActivate(ui::Pointer&, automat::Toy*);
  DEF_END(clone);

  DEF_INTERFACE(Location, Command, remove, "Delete")
  static constexpr bool kSchedulesNext = false;
  void OnRun(std::unique_ptr<RunTask>&);
  DEF_END(remove);

  DEF_INTERFACE(Location, Command, iconify, "Iconify")
  static constexpr bool kSchedulesNext = false;
  void OnRun(std::unique_ptr<RunTask>&) { obj->Iconify(); }
  DEF_END(iconify);

  DEF_INTERFACE(Location, Command, deiconify, "Deiconify")
  static constexpr bool kSchedulesNext = false;
  void OnRun(std::unique_ptr<RunTask>&) { obj->Deiconify(); }
  DEF_END(deiconify);

  DEF_INTERFACE(Location, Command, make_home, "Make Home")
  static constexpr bool kSchedulesNext = false;
  void OnRun(std::unique_ptr<RunTask>&) { obj->object->MakeHome(*obj); }
  DEF_END(make_home);

  INTERFACES(move, copy, clone, remove, iconify, deiconify, make_home)

  explicit Location(WeakPtr<Board> board = {});
  ~Location();

  Ptr<Board> LockBoard() const;

  // Call this when the object's shape or position change.
  //
  // This will wake the toys of this and all connected locations.
  void InvalidateConnectionWidgets(bool moved, bool value_changed) const;

  Ptr<Object> InsertHere(Ptr<Object>&& object);

  Ptr<Object> Create(const Object& prototype);

  template <typename T, typename... Args>
  Ptr<T> Create(Args&&... args) {
    auto typed = MAKE_PTR(T, std::forward<Args>(args)...);
    object = typed;
    return typed;
  }

  // Remove the objects held by this location.
  //
  // Some containers may not allow empty locations so this function may also
  // delete the location. Check the return value.
  Location* Clear() {
    object.Reset();
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
  void Updated(WeakPtr<Object>& updated) override { object->Updated(updated); }

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

  void SetText(std::string_view text) override {
    std::string current_text = GetText();
    if (current_text == text) {
      return;
    }
    Follow()->SetText(text);
    ScheduleUpdate();
  }
  void SetNumber(double number);

  Ptr<Object> Clone() const override;
};

struct LocationWidget : ObjectToy {
  constexpr static float kPositionSpringPeriod = 0.2;
  constexpr static float kScaleSpringPeriod = 0.3;
  constexpr static float kSpringHalfTime = kScaleSpringPeriod / 4;

  // Animation state (moved from Location)
  animation::SpringV2<float> elevation;
  Vec2 position_vel = {};
  float scale_vel = 0;
  MortalPtr<ObjectToy> toy;  // cached Object Toy (auto-nulled on destruction)

  // Set while the widget is dragged outside of a Board (toy is pointer-owned vs board-owned)
  std::unique_ptr<Toy> owned_toy;

  // A merged copy: its location is the resident it flies into, fading out; dies on arrival.
  bool merging = false;

  float local_to_parent_weight_target = 1;

  MortalList<LocationWidget> overlapping_above;
  MortalList<LocationWidget> overlapping_below;
  Rect stack_draw_bounds;
  uint32_t overlap_genid = 0;
  int overlap_zindex = -1;

  Rect CoverBounds() const override { return stack_draw_bounds; }

  // Keep Toy in the board's ToyScope.
  static std::unique_ptr<LocationWidget> MakeBoardOwned(ui::Widget* parent, Location& loc);
  // Keep Toy owned locally.
  static std::unique_ptr<LocationWidget> MakePointerOwned(ui::Widget* parent, Location& loc,
                                                          std::unique_ptr<Toy>&& toy = nullptr);

  Ptr<Location> LockLocation() const { return LockObject<Location>(); }

  ObjectToy& ToyForObject();
  Vec2 LocalAnchor() const override;

  ui::Widget::TextureAnchor* GrabAnchor() const;

  void AnchorToPointer(ui::Pointer&, Vec2 grab);

  // Widget overrides
  Tock Tick(time::Timer& timer) override;
  void Draw(SkCanvas&) const override;
  SkPath Shape() const override;
  Optional<Rect> DrawBounds() const override;
  Interface FindOption(ui::Pointer&, ui::ActionTrigger) override;
  void FillMenu(ui::Pointer&, Menu&) override;

  void OnPoll(time::Timer& timer) override {
    if (owned_toy) owned_toy->Poll(timer);
  }

  // Call this when the position of this location changes to update the autoconnect arguments.
  void UpdateAutoconnectArgs();

  void OnReparent(ui::Widget& new_parent, SkM44& fix) override;
  void OnChildReparentedAway(ui::Widget& child) override;

 private:
  LocationWidget(ui::Widget* parent, Location& loc);
};

static_assert(ToyMaker<Location>);

void PositionBelow(Location& origin, Location& below);

void AppendObscurers(Location* loc, Location* other_end, Vec<ui::Widget*>& wanted);

// Return position for the given `target_widget` ahead of the `origin`s `arg`.
//
// This uses the arg's position & direction within `origin`.
//
// This is a UI function.
Vec2 PositionAhead(ObjectToy& origin_toy, const Argument::Table& arg,
                   const ObjectToy& target_widget);

// Return position for the given `target_widget` beside the `origin` (to its right, tops
// aligned), stepping down past locations already sitting there.
//
// This is a UI function.
Vec2 PositionBeside(Location& origin, Location& target, const ObjectToy& target_widget);

// Engine function for animating location appearance.
//
// Not implemented ATM
void AnimateGrowFrom(Location& source, Location& grown);

}  // namespace automat

#include "board.hpp"
