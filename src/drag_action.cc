#include "drag_action.hh"

#include <cmath>

#include "action.hh"
#include "animation.hh"
#include "gui_connection_widget.hh"
#include "math.hh"
#include "pointer.hh"
#include "root.hh"
#include "window.hh"

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

static gui::DropTarget* FindDropTarget(DragLocationAction& a) {
  using namespace gui;
  using namespace maf;

  Path path;
  Vec2 point = a.pointer.pointer_position;
  auto* display = &a.pointer.window.display;

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

  struct Widget* window_arr[] = {a.pointer.path[0]};
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

  if (last_snapped_position != position) {
    last_snapped_position = position;
    location->animation_state[pointer.window.display].position.value +=
        current_position - last_position;
  }
  location->scale = scale;
  location->position = position;

  last_position = current_position;
}

void DragLocationAction::End() {
  if (gui::DropTarget* drop_target = FindDropTarget(*this)) {
    drop_target->DropLocation(location);
  }
}

DragLocationAction::DragLocationAction(gui::Pointer& pointer, Location* location)
    : Action(pointer), location(location) {
  pointer.window.drag_action_count++;
  // Go over every ConnectionWidget and see if any of its arguments can be connected to this object.
  // Set their "radar" to 1
  for (auto& connection_widget : root_machine->connection_widgets) {
    if (&connection_widget->from == location) {
      connection_widget->animation_state[pointer.window.display].radar_alpha_target = 1;
    } else {
      string error;
      connection_widget->arg.CheckRequirements(connection_widget->from, location,
                                               DraggedObject(*this), error);
      if (error.empty()) {
        connection_widget->animation_state[pointer.window.display].radar_alpha_target = 1;
      }
    }
  }
}

DragLocationAction::~DragLocationAction() {
  pointer.window.drag_action_count--;
  for (auto& connection_widget : root_machine->connection_widgets) {
    connection_widget->animation_state[pointer.window.display].radar_alpha_target = 0;
  }
}

}  // namespace automat
