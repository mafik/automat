// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "drag_action.hh"

#include <include/core/SkPath.h>

#include <cmath>

#include "action.hh"
#include "animation.hh"
#include "gui_connection_widget.hh"
#include "math.hh"
#include "pointer.hh"
#include "root_widget.hh"

using namespace maf;
using namespace automat::gui;

namespace automat {

static Vec2 RoundToMilimeters(Vec2 v) {
  return Vec2(roundf(v.x * 1000) / 1000., roundf(v.y * 1000) / 1000.);
}

static Object* DraggedObject(DragLocationAction& a) { return a.location->object.get(); }

Vec2 SnapPosition(DragLocationAction& d) {
  return RoundToMilimeters(d.current_position - d.contact_point);
}

static gui::DropTarget* FindDropTarget(DragLocationAction& a, Widget& widget) {
  for (auto& child : widget.Children()) {
    if (auto drop_target = FindDropTarget(a, *child)) {
      return drop_target;
    }
  }
  Vec2 point = a.pointer.PositionWithin(widget);
  if ((widget.pack_frame_texture_bounds == std::nullopt) ||
      widget.Shape().contains(point.x, point.y)) {
    if (auto drop_target = widget.AsDropTarget()) {
      if (drop_target->CanDrop(*a.location)) {
        return drop_target;
      }
    }
  }
  return nullptr;
}

static gui::DropTarget* FindDropTarget(DragLocationAction& a) {
  return FindDropTarget(a, a.pointer.root_widget);
}

void DragLocationAction::Update() {
  current_position = pointer.PositionWithinRootMachine();

  Vec2 position = current_position - contact_point;
  float scale = 1;

  if (gui::DropTarget* drop_target = FindDropTarget(*this)) {
    drop_target->SnapPosition(position, scale, *location, &contact_point);
  }

  location->scale = scale;
  location->position = position;

  if (last_snapped_position != position) {
    last_snapped_position = position;
    location->animation_state.position.value += current_position - last_position;
    location->UpdateAutoconnectArgs();
    location->WakeAnimation();
    location->InvalidateConnectionWidgets(true, false);
  }

  last_position = current_position;
}

SkPath DragLocationWidget::Shape() const { return SkPath(); }

DragLocationAction::DragLocationAction(gui::Pointer& pointer, Ptr<Location>&& location_arg,
                                       Vec2 contact_point)
    : Action(pointer),
      contact_point(contact_point),
      location(std::move(location_arg)),
      widget(MakePtr<DragLocationWidget>(*this)) {
  widget->parent = pointer.root_widget.SharedPtr();
  pointer.root_widget.drag_action_count++;
  location->parent = widget;
  widget->FixParents();
  // Go over every ConnectionWidget and see if any of its arguments can be connected to this
  // object. Set their "radar" to 1
  for (auto& connection_widget : gui::root_widget->connection_widgets) {
    if (&connection_widget->from == location.get()) {
      connection_widget->animation_state.radar_alpha_target = 1;
    } else {
      string error;
      connection_widget->arg.CheckRequirements(connection_widget->from, location.get(),
                                               DraggedObject(*this), error);
      if (error.empty()) {
        connection_widget->animation_state.radar_alpha_target = 1;
      }
    }
  }
  gui::root_widget->WakeAnimation();

  last_position = current_position = pointer.PositionWithinRootMachine();
  Update();
}

DragLocationAction::~DragLocationAction() {
  gui::DropTarget* drop_target = FindDropTarget(*this);
  if (drop_target) {
    location->WakeAnimation();
    drop_target->DropLocation(std::move(location));
  }

  pointer.root_widget.drag_action_count--;
  for (auto& connection_widget : gui::root_widget->connection_widgets) {
    connection_widget->animation_state.radar_alpha_target = 0;
  }
  if (location) {
    location->ForgetParents();
  }
  gui::root_widget->WakeAnimation();
}
void DragLocationWidget::FillChildren(maf::Vec<Ptr<Widget>>& children) {
  children.push_back(action.location);
}

}  // namespace automat
