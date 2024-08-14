#pragma once
#include <include/core/SkPath.h>

#include <memory>

#include "action.hh"
#include "animation.hh"
#include "object.hh"
#include "time.hh"

namespace automat {

struct Location;
struct ObjectAnimationState;

namespace gui {
struct DropTarget {
  virtual void SnapPosition(Vec2& position, float& scale, Object* object,
                            Vec2* fixed_point = nullptr) = 0;

  virtual void DropObject(
      std::unique_ptr<Object>&& object, Vec2 position, float scale,
      std::unique_ptr<animation::PerDisplay<ObjectAnimationState>>&& animation_state) = 0;

  // When a location is being dragged around, its still owned by its original Machine. Only when
  // this method is called, the location may be re-parented into the new drop target.
  // The drop target is responsible for re-parenting the location!
  virtual void DropLocation(Location* location) = 0;
};
}  // namespace gui

struct DragLocationAction : Action {
  Vec2 contact_point;          // in the coordinate space of the dragged Object
  Vec2 last_position;          // root machine coordinates
  Vec2 current_position;       // root machine coordinates
  Vec2 last_snapped_position;  // root machine coordinates
  time::SteadyPoint last_update;
  Location* location;

  DragLocationAction(gui::Pointer&, Location*);
  ~DragLocationAction() override;

  void Begin() override;
  void Update() override;
  void End() override;
};

}  // namespace automat