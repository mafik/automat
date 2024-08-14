#include "drag_action.hh"

#include <cmath>

#include "action.hh"
#include "animation.hh"
#include "gui_connection_widget.hh"
#include "log.hh"
#include "math.hh"
#include "pointer.hh"
#include "root.hh"
#include "window.hh"

namespace automat {

static Vec2 RoundToMilimeters(Vec2 v) {
  return Vec2(roundf(v.x * 1000) / 1000., roundf(v.y * 1000) / 1000.);
}

void DragActionBase::Begin() {
  last_position = current_position = pointer.PositionWithinRootMachine();
  Update();
}

Vec2 SnapPosition(DragActionBase& d) {
  return RoundToMilimeters(d.current_position - d.contact_point);
}

void DragActionBase::Update() {
  current_position = pointer.PositionWithinRootMachine();

  Vec2 position = current_position - contact_point;
  float scale = 1;

  if (gui::DropTarget* drop_target = FindDropTarget()) {
    drop_target->SnapPosition(position, scale, DraggedObject(), &contact_point);
  }

  if (last_snapped_position != position) {
    last_snapped_position = position;
    DragUpdate(pointer.window.display, current_position - last_position);
  }
  SnapUpdate(position, scale);

  last_position = current_position;
}

void DragLocationAction::DragUpdate(animation::Display& display, Vec2 delta_pos) {
  location->animation_state[display].position.value += delta_pos;
}

void DragLocationAction::SnapUpdate(Vec2 pos, float scale) {
  location->scale = scale;
  location->position = pos;
}

void DragActionBase::End() { DragEnd(); }

gui::DropTarget* DragActionBase::FindDropTarget() {
  using namespace gui;
  using namespace maf;

  Path path;
  Vec2 point = pointer.pointer_position;
  auto* display = &pointer.window.display;

  gui::DropTarget* drop_target = nullptr;

  Visitor dfs = [&](Span<struct Widget*> widgets) -> ControlFlow {
    for (auto w : widgets) {
      Vec2 transformed;
      if (!path.empty()) {
        transformed = path.back()->TransformToChild(*w, display).mapPoint(point);
      } else {
        transformed = point;
      }

      auto shape = w->Shape(display);
      path.push_back(w);
      std::swap(point, transformed);
      if (w->ChildrenOutside() || shape.contains(point.x, point.y)) {
        if (w->VisitChildren(dfs) == ControlFlow::Stop) {
          return ControlFlow::Stop;
        }
        if ((drop_target = w->CanDrop())) {
          return ControlFlow::Stop;
        }
      }
      std::swap(point, transformed);
      path.pop_back();
    }
    return ControlFlow::Continue;
  };

  struct Widget* window_arr[] = {pointer.path[0]};
  dfs(window_arr);
  return drop_target;
}

DragLocationAction::DragLocationAction(gui::Pointer& pointer, Location* location)
    : DragActionBase(pointer), location(location) {
  // Go over every ConnectionWidget and see if any of its arguments can be connected to this object.
  // Set their "radar" to 1
  for (auto& connection_widget : root_machine->connection_widgets) {
    if (&connection_widget->from == location) {
      connection_widget->animation_state[pointer.window.display].radar_alpha_target = 1;
    } else {
      string error;
      connection_widget->arg.CheckRequirements(connection_widget->from, location, DraggedObject(),
                                               error);
      if (error.empty()) {
        connection_widget->animation_state[pointer.window.display].radar_alpha_target = 1;
      }
    }
  }
}

DragLocationAction::~DragLocationAction() {
  for (auto& connection_widget : root_machine->connection_widgets) {
    connection_widget->animation_state[pointer.window.display].radar_alpha_target = 0;
  }
}

void DragLocationAction::DragEnd() {
  if (gui::DropTarget* drop_target = FindDropTarget()) {
    drop_target->DropLocation(location);
  }
}

Object* DragLocationAction::DraggedObject() { return location->object.get(); }

DragActionBase::DragActionBase(gui::Pointer& pointer) : Action(pointer) {
  pointer.window.drag_action_count++;
}

DragActionBase::~DragActionBase() { pointer.window.drag_action_count--; }

}  // namespace automat
