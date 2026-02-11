// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once
#include <include/core/SkMatrix.h>
#include <include/core/SkPath.h>

#include <optional>

#include "action.hh"
#include "object.hh"
#include "time.hh"

namespace automat {

struct Location;
struct LocationWidget;

namespace ui {

// Interface for widgets that can receive locations being dropped on them.
struct DropTarget {
  virtual bool CanDrop(Location&) const = 0;

  // Snap the given Rect, which is hovered over this drop target.
  // bounds_origin is a point that should be used for center-aligned snapping.
  // Optionally respecting a "fixed point".
  virtual SkMatrix DropSnap(const Rect& bounds, Vec2 bounds_origin,
                            Vec2* fixed_point = nullptr) = 0;

  // When a location is being dragged around, its still owned by its original Board. Only when
  // this method is called, the location may be re-parented into the new drop target.
  // The drop target is responsible for re-parenting the location!
  virtual void DropLocation(Ptr<Location>&&) = 0;
};
}  // namespace ui

struct DragLocationAction;

struct DragLocationWidget : ui::Widget {
  DragLocationAction& action;
  DragLocationWidget(ui::Widget* parent, DragLocationAction& action)
      : ui::Widget(parent), action(action) {}
  SkPath Shape() const override;
  Optional<Rect> TextureBounds() const override { return std::nullopt; }
};

struct DragLocationAction : Action {
  Vec2 last_position;          // root board coordinates
  Vec2 current_position;       // root board coordinates
  Vec2 last_snapped_position;  // root board coordinates
  time::SteadyPoint last_update;
  Vec<Ptr<Location>> locations;
  unique_ptr<DragLocationWidget> widget;

  DragLocationAction(ui::Pointer&, Ptr<Location>&&);
  DragLocationAction(ui::Pointer&, Vec<Ptr<Location>>&&);
  ~DragLocationAction() override;

  void Update() override;
  ui::Widget* Widget() override { return widget.get(); }

  void VisitObjects(std::function<void(Object&)>) override;
};

bool IsDragged(const LocationWidget& location);

}  // namespace automat
