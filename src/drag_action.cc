// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "drag_action.hh"

#include <include/core/SkPath.h>

#include <cmath>
#include <ranges>

#include "action.hh"
#include "animation.hh"
#include "automat.hh"
#include "log_skia.hh"
#include "math.hh"
#include "pointer.hh"
#include "root_widget.hh"
#include "ui_connection_widget.hh"

using namespace automat::ui;

namespace automat {

static Vec2 RoundToMilimeters(Vec2 v) {
  return Vec2(roundf(v.x * 1000) / 1000., roundf(v.y * 1000) / 1000.);
}

static ui::DropTarget* FindDropTarget(DragLocationAction& a, Widget& widget) {
  for (auto* child : widget.Children()) {
    if (auto drop_target = FindDropTarget(a, *child)) {
      return drop_target;
    }
  }
  Vec2 point = a.pointer.PositionWithin(widget);
  auto shape = widget.Shape();
  if (shape.isEmpty() || shape.contains(point.x, point.y)) {
    if (auto drop_target = widget.AsDropTarget()) {
      if (drop_target->CanDrop(*a.locations.back())) {
        return drop_target;
      }
    }
  }
  return nullptr;
}

static ui::DropTarget* FindDropTarget(DragLocationAction& a) {
  return FindDropTarget(a, a.pointer.root_widget);
}

void DragLocationAction::Update() {
  current_position = pointer.PositionWithinRootMachine();

  int n = locations.size();
  Toy* widgets[n];
  for (int i = 0; i < n; ++i) {
    widgets[i] = &locations[i]->ToyForObject();
  }
  Rect location_bounds[n];
  for (int i = 0; i < n; ++i) {
    location_bounds[i] = widgets[i]->CoarseBounds().rect;
  }
  SkMatrix location_transform[n];
  for (int i = 0; i < n; ++i) {
    float scale = locations[i]->widget->toy->GetBaseScale();
    location_transform[i] = SkMatrix::Scale(scale, scale)
                                .postTranslate(current_position.x, current_position.y)
                                .preTranslate(-locations[i]->widget->local_anchor->x,
                                              -locations[i]->widget->local_anchor->y);
  }

  Vec2 bounds_origin;
  if (widgets[n - 1]->CenteredAtZero()) {
    bounds_origin = location_transform[n - 1].mapOrigin();
  } else {
    bounds_origin = location_transform[n - 1].mapPoint(location_bounds[n - 1].Center());
  }

  for (int i = 0; i < n; ++i) {
    location_transform[i].mapRect(&location_bounds[i].sk);
  }

  Rect bounds_all = location_bounds[0];
  for (int i = 1; i < n; ++i) {
    bounds_all.ExpandToInclude(location_bounds[i]);
  }

  SkMatrix snap = {};
  if (ui::DropTarget* drop_target = FindDropTarget(*this)) {
    snap = drop_target->DropSnap(bounds_all, bounds_origin, &current_position);
  }

  bool moved = false;
  for (int i = 0; i < n; ++i) {
    location_transform[i].postConcat(snap);
    Vec2 new_position;
    float new_scale;
    Location::FromMatrix(location_transform[i], locations[i]->widget->LocalAnchor(), new_position,
                         new_scale);
    if (!NearlyEqual(new_position, locations[i]->position)) {
      moved = true;
      Vec2 fix = current_position - last_position;
      widgets[i]->local_to_parent.postTranslate(fix.x, fix.y);
    }
    locations[i]->position = new_position;
    locations[i]->scale = new_scale;
  }

  if (moved) {
    for (auto& location : locations) {
      if (location->widget) location->widget->UpdateAutoconnectArgs();
    }
    for (auto& location : locations) {
      location->WakeToys();
      if (location->widget) location->widget->InvalidateConnectionWidgets(true, false);
    }
  }

  last_position = current_position;
}

SkPath DragLocationWidget::Shape() const { return SkPath(); }

DragLocationAction::DragLocationAction(ui::Pointer& pointer, Vec<Ptr<Location>>&& locations_arg)
    : Action(pointer),
      locations(std::move(locations_arg)),
      widget(new DragLocationWidget(pointer.GetWidget(), *this)) {
  auto& root = pointer.root_widget;
  root.drag_action_count++;
  if (root.drag_action_count == 1) {
    root.black_hole.WakeAnimation();
  }
  for (auto& location : locations) {
    auto* lw = static_cast<LocationWidget*>(root.toys.FindOrNull(*location));
    if (lw == nullptr) {
      ERROR << "DragLocationAction for Location without a Widget";
      continue;
    }
    auto fix = TransformBetween(*lw, *widget);

    auto target_matrix = Location::ToMatrix(location->position, location->scale, lw->LocalAnchor());
    target_matrix.postConcat(fix);

    auto& toy = location->ToyForObject();
    toy.local_to_parent.postConcat(SkM44(fix));
    {  // Transform velocities
      fix.mapVector(lw->position_vel);
      fix.mapRadius(lw->scale_vel);
    }

    lw->parent = widget.get();
    lw->local_anchor = pointer.PositionWithin(toy);
    location->WakeToys();
  }
  widget->ValidateHierarchy();
  widget->RedrawThisFrame();

  // Go over every ConnectionWidget that starts / ends in the dragged stack & update its parent
  for (auto& connection_widget : ui::root_widget->connection_widgets) {
    auto arg = connection_widget->start_weak.Lock();
    if (!arg) continue;
    auto& start = *arg.Owner<Object>();
    auto* start_loc = start.here;
    Location* end_loc = nullptr;
    if (auto target = arg->Find(start)) {
      auto& end = *target.Owner<Object>();
      end_loc = end.here;
    }

    for (auto& location : locations) {
      if (start_loc == location.Get() || end_loc == location.Get()) {
        if (location->widget) ReparentConnectionWidget(*connection_widget, *location->widget);
      }
    }
  }

  // Go over every ConnectionWidget and set their "radar" to 1, if needed.
  for (auto& connection_widget : ui::root_widget->connection_widgets) {
    auto arg = connection_widget->start_weak.Lock();
    if (!arg) continue;
    auto& start = *arg.Owner<Object>();

    for (auto& location : locations) {
      if (&start == location->object.Get()) {
        // We grabbed the start object of this connection widget
        connection_widget->animation_state.radar_alpha_target = 1;
      } else {
        // This connection widget can be connected to one of dragged locations
        if (arg->CanConnect(start, *location->object)) {
          connection_widget->animation_state.radar_alpha_target = 1;
        }
      }
    }
  }
  ui::root_widget->WakeAnimation();

  last_position = current_position = pointer.PositionWithinRootMachine();

  Update();
}

DragLocationAction::DragLocationAction(ui::Pointer& pointer, Ptr<Location>&& location_arg)
    : DragLocationAction(pointer, MakeVec<Ptr<Location>>(std::move(location_arg))) {}

DragLocationAction::~DragLocationAction() {
  ui::DropTarget* drop_target = FindDropTarget(*this);
  if (drop_target) {
    for (auto& location : std::ranges::reverse_view(locations)) {
      location->WakeToys();
      {  // Use matrix to keep the object in place while clearing the local anchor
        auto matrix = Location::ToMatrix(location->position, location->scale,
                                         *location->widget->local_anchor);
        location->widget->local_anchor.reset();
        Location::FromMatrix(matrix, location->widget->LocalAnchor(), location->position,
                             location->scale);
      }
      drop_target->DropLocation(std::move(location));
    }
  }

  pointer.root_widget.drag_action_count--;
  for (auto& connection_widget : ui::root_widget->connection_widgets) {
    connection_widget->animation_state.radar_alpha_target = 0;
  }
  ui::root_widget->WakeAnimation();
}

void DragLocationWidget::FillChildren(Vec<Widget*>& children) {
  for (auto& location : action.locations) {
    if (location->widget) children.push_back(location->widget);
  }
}

bool IsDragged(const Location& location) {
  return location.widget &&
         dynamic_cast<const DragLocationWidget*>(location.widget->parent.get()) != nullptr;
}

}  // namespace automat
