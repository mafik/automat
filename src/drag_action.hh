// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once
#include <include/core/SkPath.h>

#include <memory>
#include <optional>

#include "action.hh"
#include "animation.hh"
#include "control_flow.hh"
#include "object.hh"
#include "time.hh"

namespace automat {

struct Location;
struct ObjectAnimationState;

namespace gui {
struct DropTarget {
  virtual void SnapPosition(Vec2& position, float& scale, Object* object,
                            Vec2* fixed_point = nullptr) = 0;

  // When a location is being dragged around, its still owned by its original Machine. Only when
  // this method is called, the location may be re-parented into the new drop target.
  // The drop target is responsible for re-parenting the location!
  virtual void DropLocation(std::unique_ptr<Location>&&) = 0;
};
}  // namespace gui

struct DragLocationAction : Action, gui::Widget {
  Vec2 contact_point;          // in the coordinate space of the dragged Object
  Vec2 last_position;          // root machine coordinates
  Vec2 current_position;       // root machine coordinates
  Vec2 last_snapped_position;  // root machine coordinates
  time::SteadyPoint last_update;
  std::unique_ptr<Location> location;

  DragLocationAction(gui::Pointer&, std::unique_ptr<Location>&&);
  ~DragLocationAction() override;

  SkPath Shape(animation::Display*) const override;
  ControlFlow VisitChildren(gui::Visitor& visitor) override;
  SkMatrix TransformToChild(const gui::Widget& child, animation::Display* display) const override;
  maf::Optional<Rect> TextureBounds(animation::Display*) const override { return std::nullopt; }

  void Begin() override;
  void Update() override;
  void End() override;
  gui::Widget* Widget() override { return this; }
};

}  // namespace automat