// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "drag_action.hh"

#include <include/core/SkPath.h>

#include <cmath>
#include <ranges>

#include "action.hh"
#include "animation.hh"
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
  for (auto& child : widget.Children()) {
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

  Rect bounds_all;
  for (int i = 0; i < locations.size(); ++i) {
    auto& location = locations[i];
    auto& object_widget = location->WidgetForObject();
    auto location_bounds = object_widget.CoarseBounds().rect;
    auto location_scale = location->GetBaseScale();

    auto location_transform = SkMatrix::Scale(location_scale, location_scale)
                                  .postTranslate(current_position.x, current_position.y)
                                  .preTranslate(-initial_positions[i].x, -initial_positions[i].y);

    location_transform.mapRect(&location_bounds.sk);
    if (bounds_all.sk.isEmpty()) {
      bounds_all = location_bounds;
    } else {
      bounds_all.ExpandToInclude(location_bounds);
    }
  }

  SkMatrix snap = {};
  if (ui::DropTarget* drop_target = FindDropTarget(*this)) {
    snap = drop_target->DropSnap(bounds_all, &current_position);
  }

  for (int i = 0; i < locations.size(); ++i) {
    auto& location = locations[i];
    auto& object_widget = location->WidgetForObject();
    auto location_scale = location->GetBaseScale();

    auto location_transform = SkMatrix::Scale(location_scale, location_scale)
                                  .postTranslate(current_position.x, current_position.y)
                                  .preTranslate(-initial_positions[i].x, -initial_positions[i].y);
    location_transform.postConcat(snap);
    location->position.x = location_transform.getTranslateX();
    location->position.y = location_transform.getTranslateY();
    location->scale = location_transform.getScaleX();
    location->WakeAnimation();
  }

  // If the location target position moves, apply the mouse delta to the location's widget make it
  // more responsive.
  // if (last_snapped_position != position) {
  //   last_snapped_position = position;
  //   for (auto& location : locations) {
  //     Vec2 fix = current_position - last_position;
  //     location->object_widget->local_to_parent.postTranslate(fix.x, fix.y);
  //   }
  //   for (auto& location : locations) {
  //     location->UpdateAutoconnectArgs();
  //   }
  //   for (auto& location : locations) {
  //     location->WakeAnimation();
  //     location->InvalidateConnectionWidgets(true, false);
  //   }
  // }

  last_position = current_position;
}

SkPath DragLocationWidget::Shape() const { return SkPath(); }

DragLocationAction::DragLocationAction(ui::Pointer& pointer, Vec<Ptr<Location>>&& locations_arg)
    : Action(pointer),
      locations(std::move(locations_arg)),
      widget(new DragLocationWidget(pointer.GetWidget(), *this)) {
  pointer.root_widget.drag_action_count++;
  if (pointer.root_widget.drag_action_count == 1) {
    pointer.root_widget.black_hole.WakeAnimation();
  }
  for (auto& location : locations) {
    auto fix = SkM44(TransformBetween(*location, *widget));

    auto new_position = fix.map(location->position.x, location->position.y, 0, 1);
    location->position = Vec2(new_position.x, new_position.y);

    auto& object_widget = location->WidgetForObject();
    object_widget.local_to_parent.postConcat(fix);
    // clear translation
    fix.setRC(0, 3, 0);
    fix.setRC(1, 3, 0);
    fix.setRC(2, 3, 0);
    location->local_to_parent_velocity.postConcat(fix);

    location->parent = widget.get();
    location->scale_pivot = pointer.PositionWithin(object_widget);
    location->WakeAnimation();
    initial_positions.push_back(pointer.PositionWithin(object_widget));
  }
  widget->ValidateHierarchy();
  widget->RedrawThisFrame();
  // Go over every ConnectionWidget and see if any of its arguments can be connected to this
  // object. Set their "radar" to 1
  for (auto& connection_widget : ui::root_widget->connection_widgets) {
    // Do this for every dragged location
    for (auto& location : locations) {
      if (&connection_widget->from == location.get()) {
        connection_widget->animation_state.radar_alpha_target = 1;
      } else {
        string error;
        connection_widget->arg.CheckRequirements(connection_widget->from, location.get(),
                                                 location->object.get(), error);
        if (error.empty()) {
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
      location->WakeAnimation();
      location->scale_pivot.reset();
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
    children.push_back(location.get());
  }
}

bool IsDragged(const Location& location) {
  return dynamic_cast<const DragLocationWidget*>(location.parent.get()) != nullptr;
}

}  // namespace automat
