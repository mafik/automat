// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once
#include <include/core/SkPath.h>

#include <optional>

#include "action.hh"
#include "object.hh"
#include "time.hh"

namespace automat {

struct Location;

namespace ui {

// Interface for widgets that can receive locations being dropped on them.
struct DropTarget {
  virtual bool CanDrop(Location&) const = 0;

  // Snap the location & scale of the given location, which is hovered over this drop target.
  // Optionally respecting a "fixed point", held by the pointer.
  // TODO: maybe make this func work in local coords?
  virtual void SnapPosition(Vec2& position, float& scale, Location&,
                            Vec2* fixed_point = nullptr) = 0;

  // When a location is being dragged around, its still owned by its original Machine. Only when
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
  void FillChildren(Vec<Widget*>& children) override;
  Optional<Rect> TextureBounds() const override { return std::nullopt; }
};

struct DragLocationAction : Action {
  Vec2 contact_point;          // in the coordinate space of the dragged Object
  Vec2 last_position;          // root machine coordinates
  Vec2 current_position;       // root machine coordinates
  Vec2 last_snapped_position;  // root machine coordinates
  time::SteadyPoint last_update;
  Vec<Ptr<Location>> locations;
  unique_ptr<DragLocationWidget> widget;

  DragLocationAction(ui::Pointer&, Ptr<Location>&&, Vec2 contact_point);
  DragLocationAction(ui::Pointer&, Vec<Ptr<Location>>&&, Vec2 contact_point);
  ~DragLocationAction() override;

  void Update() override;
  ui::Widget* Widget() override { return widget.get(); }
};

bool IsDragged(const Location& location);

}  // namespace automat