// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "argument.hh"

#include <cmath>

#include "automat.hh"
#include "base.hh"
#include "drag_action.hh"
#include "gui_connection_widget.hh"
#include "root_widget.hh"
#include "svg.hh"
#include "widget.hh"

namespace automat {

Argument next_arg("next", Argument::kOptional);

Argument::FinalLocationResult Argument::GetFinalLocation(
    Location& here, std::source_location source_location) const {
  FinalLocationResult result(GetObject(here, source_location));
  if (auto live_object = dynamic_cast<LiveObject*>(result.object)) {
    result.final_location = live_object->here.lock().get();
  }
  return result;
}

Argument::LocationResult Argument::GetLocation(Location& here,
                                               std::source_location source_location) const {
  LocationResult result;
  auto conn_it = here.outgoing.find(this);
  if (conn_it != here.outgoing.end()) {  // explicit connection
    auto* c = *conn_it;
    result.location = &c->to;
    result.follow_pointers = c->pointer_behavior == Connection::kFollowPointers;
  }
  if (result.location == nullptr && precondition >= kRequiresLocation) {
    here.ReportError(f("The {} argument of {} is not connected.", name, here.ToStr()),
                     source_location);
    result.ok = false;
  }
  return result;
}

Argument::ObjectResult Argument::GetObject(Location& here,
                                           std::source_location source_location) const {
  ObjectResult result(GetLocation(here, source_location));
  if (result.location) {
    if (result.follow_pointers) {
      result.object = result.location->Follow();
    } else {
      result.object = result.location->object.get();
    }
    if (result.object == nullptr && precondition >= kRequiresObject) {
      here.ReportError(f("The {} argument of {} is empty.", name, here.ToStr()), source_location);
      result.ok = false;
    }
  }
  return result;
}

PaintDrawable& Argument::Icon() {
  static DrawableSkPath default_icon = []() {
    SkPath path = PathFromSVG(kNextShape);
    return DrawableSkPath(path);
  }();
  return default_icon;
}
bool Argument::IsOn(Location& here) const {
  if (field) {
    if (auto on_off = dynamic_cast<OnOff*>(field)) {
      return on_off->IsOn();
    }
  }
  return false;
}

#pragma region New API

Vec2AndDir Argument::Start(gui::Widget& object_widget, gui::Widget& widget) const {
  auto pos_dir = object_widget.ArgStart(*this);
  auto m = TransformBetween(object_widget, widget);
  pos_dir.pos = m.mapPoint(pos_dir.pos);
  return pos_dir;
}

void Argument::NearbyCandidates(
    Location& here, float radius,
    std::function<void(Location&, Vec<Vec2AndDir>& to_points)> callback) const {
  // Check the currently dragged object
  auto& root_widget = here.FindRootWidget();
  for (auto* action : root_widget.active_actions) {
    if (auto* drag_location_action = dynamic_cast<DragLocationAction*>(action)) {
      for (auto& location : drag_location_action->locations) {
        if (location.get() == &here) {
          continue;
        }
        std::string error;
        for (auto& req : requirements) {
          req(location.get(), location->object.get(), error);
          if (!error.empty()) {
            break;
          }
        }
        if (!error.empty()) {
          continue;
        }
        Vec<Vec2AndDir> to_points;
        location->WidgetForObject().ConnectionPositions(to_points);
        callback(*location, to_points);
      }
    }
  }
  // Query nearby objects in the parent machine

  Vec2 center = this->Start(here.WidgetForObject(), *root_machine).pos;
  root_machine->Nearby(center, radius, [&](Location& other) -> void* {
    if (&other == &here) {
      return nullptr;
    }
    for (auto& req : requirements) {
      std::string error;
      req(&other, other.object.get(), error);
      if (!error.empty()) {
        return nullptr;
      }
    }
    Vec<Vec2AndDir> to_points;
    other.WidgetForObject().ConnectionPositions(to_points);
    callback(other, to_points);
    return nullptr;
  });
}

Location* Argument::FindLocation(Location& here, const FindConfig& cfg) const {
  auto conn_it = here.outgoing.find(this);
  Location* result = nullptr;
  if (conn_it != here.outgoing.end()) {  // explicit connection
    auto* c = *conn_it;
    result = &c->to;
  }
  if (result == nullptr && cfg.if_missing == IfMissing::CreateFromPrototype) {
    // Ask the current location for the prototype for this object.
    if (auto prototype = here.object->ArgPrototype(*this)) {
      if (auto machine = here.ParentAs<Machine>()) {
        Location& l = machine->Create(*prototype);
        result = &l;
        Rect object_bounds = l.WidgetForObject().Shape().getBounds();
        l.position =
            here.position + here.WidgetForObject().ArgStart(*this).pos - object_bounds.TopCenter();
        PositionBelow(here, l);
        AnimateGrowFrom(here,
                        l);  // this must go before UpdateAutoconnectArgs because of animation_state
        l.UpdateAutoconnectArgs();
        l.WakeAnimation();
      }
    }
  }
  return result;
}

Object* Argument::FindObject(Location& here, const FindConfig& cfg) const {
  if (auto loc = FindLocation(here, cfg)) {
    return loc->object.get();
  }
  return nullptr;
}

void LiveArgument::Detach(Location& here) {
  auto connections = here.outgoing.equal_range(this);
  for (auto it = connections.first; it != connections.second; ++it) {
    auto& connection = *it;
    here.StopObservingUpdates(connection->to);
  }
}

void LiveArgument::Attach(Location& here) {
  auto connections = here.outgoing.equal_range(this);
  for (auto it = connections.first; it != connections.second; ++it) {
    auto* connection = *it;
    here.ObserveUpdates(connection->to);
  }
}

void Argument::InvalidateConnectionWidgets(Location& here) const {
  for (auto& w : gui::ConnectionWidgetRange(here, *this)) {
    w.WakeAnimation();
    if (w.state) {
      w.state->stabilized = false;
    }
  }
}

}  // namespace automat