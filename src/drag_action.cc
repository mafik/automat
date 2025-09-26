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

Vec2 SnapPosition(DragLocationAction& d) {
  return RoundToMilimeters(d.current_position - d.contact_point);
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

  Vec2 position = current_position - contact_point;
  float scale = 1;
  auto& base = *locations.back();
  Vec2 base_pivot = base.ScalePivot();

  if (ui::DropTarget* drop_target = FindDropTarget(*this)) {
    drop_target->SnapPosition(position, scale, base, &base_pivot);
  }

  auto old_position = base.position;
  auto old_scale = base.scale;

  base.position = position;
  base.scale = scale;

  for (int i = locations.size() - 2; i >= 0; --i) {
    auto& atop = *locations[i];
    Vec2 atop_pivot = atop.ScalePivot();
    Vec2 old_delta = atop.position + atop_pivot - old_position - base_pivot;
    Vec2 new_delta = old_delta / old_scale * scale;
    atop.position = position + base_pivot + new_delta - atop_pivot;
    atop.scale = scale;
  }

  if (last_snapped_position != position) {
    last_snapped_position = position;
    for (auto& location : locations) {
      location->animation_state.position.value += current_position - last_position;
    }
    for (auto& location : locations) {
      location->UpdateAutoconnectArgs();
    }
    for (auto& location : locations) {
      location->WakeAnimation();
      location->InvalidateConnectionWidgets(true, false);
    }
  }

  last_position = current_position;
}

SkPath DragLocationWidget::Shape() const { return SkPath(); }

DragLocationAction::DragLocationAction(ui::Pointer& pointer, Vec<Ptr<Location>>&& locations_arg,
                                       Vec2 contact_point)
    : Action(pointer),
      contact_point(contact_point),
      locations(std::move(locations_arg)),
      widget(new DragLocationWidget(pointer.GetWidget(), *this)) {
  pointer.root_widget.drag_action_count++;
  if (pointer.root_widget.drag_action_count == 1) {
    pointer.root_widget.black_hole.WakeAnimation();
  }
  for (auto& location : locations) {
    location->parent = widget.get();
    if (location->object_widget) {
      location->animation_state.scale_pivot =
          contact_point + locations.back()->position - location->position;
    }
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

DragLocationAction::DragLocationAction(ui::Pointer& pointer, Ptr<Location>&& location_arg,
                                       Vec2 contact_point)
    : DragLocationAction(pointer, MakeVec<Ptr<Location>>(std::move(location_arg)), contact_point) {}

DragLocationAction::~DragLocationAction() {
  ui::DropTarget* drop_target = FindDropTarget(*this);
  if (drop_target) {
    for (auto& location : std::ranges::reverse_view(locations)) {
      location->WakeAnimation();
      location->animation_state.scale_pivot.reset();
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
