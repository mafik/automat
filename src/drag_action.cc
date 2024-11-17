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

namespace automat {

static Vec2 RoundToMilimeters(Vec2 v) {
  return Vec2(roundf(v.x * 1000) / 1000., roundf(v.y * 1000) / 1000.);
}

static Object* DraggedObject(DragLocationAction& a) { return a.location->object.get(); }

void DragLocationAction::Begin() {
  widget->parent = pointer.window.SharedPtr();
  last_position = current_position = pointer.PositionWithinRootMachine();
  Update();
}

Vec2 SnapPosition(DragLocationAction& d) {
  return RoundToMilimeters(d.current_position - d.contact_point);
}

static gui::DropTarget* FindDropTarget(DragLocationAction& a) {
  using namespace gui;
  using namespace maf;

  Vec2 point = a.pointer.pointer_position;
  auto* display = &a.pointer.window.display;

  gui::DropTarget* drop_target = nullptr;

  Visitor dfs = [&](Span<std::shared_ptr<gui::Widget>> widgets) -> ControlFlow {
    for (auto w : widgets) {
      Vec2 transformed;
      if (w->parent) {
        transformed = w->parent->TransformToChild(*w, display).mapPoint(point);
      } else {
        transformed = point;
      }

      auto shape = w->Shape();
      std::swap(point, transformed);
      if ((w->TextureBounds(display) == std::nullopt) || shape.contains(point.x, point.y)) {
        if (w->VisitChildren(dfs) == ControlFlow::Stop) {
          return ControlFlow::Stop;
        }
        if ((drop_target = w->CanDrop())) {
          return ControlFlow::Stop;
        }
      }
      std::swap(point, transformed);
    }
    return ControlFlow::Continue;
  };

  std::shared_ptr<Widget> window_arr[] = {a.pointer.window.SharedPtr<Widget>()};
  dfs(window_arr);
  return drop_target;
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
    location->InvalidateConnectionWidgets();
  }

  last_position = current_position;
}

SkPath DragLocationWidget::Shape() const { return SkPath(); }

void DragLocationAction::End() {
  if (gui::DropTarget* drop_target = FindDropTarget(*this)) {
    drop_target->DropLocation(std::move(location));
  }
}

DragLocationAction::DragLocationAction(gui::Pointer& pointer,
                                       std::shared_ptr<Location>&& location_arg)
    : Action(pointer),
      location(std::move(location_arg)),
      widget(std::make_shared<DragLocationWidget>(*this)) {
  pointer.window.drag_action_count++;
  location->Widget::parent = pointer.window.SharedPtr();
  // TODO: the right way to do this is to clear the (location's - not Widget's!) parent here
  // location->parent = nullptr;
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
}
ControlFlow DragLocationWidget::VisitChildren(gui::Visitor& visitor) {
  std::shared_ptr<gui::Widget> child = action.location;
  if (visitor(maf::SpanOfArr(&child, 1)) == ControlFlow::Stop) {
    return ControlFlow::Stop;
  }
  return ControlFlow::Continue;
}
SkMatrix DragLocationWidget::TransformToChild(const gui::Widget& child,
                                              animation::Display* display) const {
  if (&child == action.location.get()) {
    return action.location->GetTransform(display);
  }
  return SkMatrix::I();
}

}  // namespace automat
