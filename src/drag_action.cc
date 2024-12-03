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
#include "window.hh"

using namespace maf;
using namespace automat::gui;

namespace automat {

static Vec2 RoundToMilimeters(Vec2 v) {
  return Vec2(roundf(v.x * 1000) / 1000., roundf(v.y * 1000) / 1000.);
}

static Object* DraggedObject(DragLocationAction& a) { return a.location->object.get(); }

void DragLocationAction::Begin() {
  last_position = current_position = pointer.PositionWithinRootMachine();
  Update();
}

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
    if (auto drop_target = widget.CanDrop()) {
      return drop_target;
    }
  }
  return nullptr;
}

static gui::DropTarget* FindDropTarget(DragLocationAction& a) {
  return FindDropTarget(a, a.pointer.window);
}

void DragLocationAction::Update() {
  current_position = pointer.PositionWithinRootMachine();

  Vec2 position = current_position - contact_point;
  float scale = 1;

  if (gui::DropTarget* drop_target = FindDropTarget(*this)) {
    drop_target->SnapPosition(position, scale, DraggedObject(*this), &contact_point);
  }

  location->scale = scale;
  location->position = position;

  if (last_snapped_position != position) {
    last_snapped_position = position;
    location->animation_state.position.value += current_position - last_position;
    location->UpdateAutoconnectArgs();
    location->InvalidateDrawCache();
    location->InvalidateConnectionWidgets(true, false);
  }

  last_position = current_position;
}

SkPath DragLocationWidget::Shape() const { return SkPath(); }

void DragLocationAction::End() {
  gui::DropTarget* drop_target = FindDropTarget(*this);
  if (drop_target) {
    drop_target->DropLocation(std::move(location));
  }
}

DragLocationAction::DragLocationAction(gui::Pointer& pointer,
                                       std::shared_ptr<Location>&& location_arg)
    : Action(pointer),
      location(std::move(location_arg)),
      widget(std::make_shared<DragLocationWidget>(*this)) {
  widget->FixParents();
  widget->parent = pointer.window.SharedPtr();
  pointer.window.drag_action_count++;
  location->Widget::parent = widget;
  // Go over every ConnectionWidget and see if any of its arguments can be connected to this
  // object. Set their "radar" to 1
  for (auto& connection_widget : gui::window->connection_widgets) {
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
}

DragLocationAction::~DragLocationAction() {
  pointer.window.drag_action_count--;
  for (auto& connection_widget : gui::window->connection_widgets) {
    connection_widget->animation_state.radar_alpha_target = 0;
  }
  if (location) {
    location->ForgetParents();
  }
}
void DragLocationWidget::FillChildren(maf::Vec<std::shared_ptr<Widget>>& children) {
  children.push_back(action.location);
}
SkMatrix DragLocationWidget::TransformToChild(const gui::Widget& child) const {
  return SkMatrix::I();
}

}  // namespace automat
