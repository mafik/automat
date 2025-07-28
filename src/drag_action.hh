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

namespace gui {

// Interface for widgets that can receive locations being dropped on them.
struct DropTarget {
  virtual bool CanDrop(Location&) const = 0;
  virtual void SnapPosition(Vec2& position, float& scale, Location&,
                            Vec2* fixed_point = nullptr) = 0;

  // When a location is being dragged around, its still owned by its original Machine. Only when
  // this method is called, the location may be re-parented into the new drop target.
  // The drop target is responsible for re-parenting the location!
  virtual void DropLocation(Ptr<Location>&&) = 0;
};
}  // namespace gui

struct DragLocationAction;

struct DragLocationWidget : gui::Widget {
  DragLocationAction& action;
  DragLocationWidget(gui::Widget& parent, DragLocationAction& action)
      : gui::Widget(parent), action(action) {}
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

  DragLocationAction(gui::Pointer&, Ptr<Location>&&, Vec2 contact_point);
  DragLocationAction(gui::Pointer&, Vec<Ptr<Location>>&&, Vec2 contact_point);
  ~DragLocationAction() override;

  void Update() override;
  gui::Widget* Widget() override { return widget.get(); }
};

bool IsDragged(const Location& location);

}  // namespace automat