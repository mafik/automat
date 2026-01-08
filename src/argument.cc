// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "argument.hh"

#include <cmath>

#include "automat.hh"
#include "base.hh"
#include "drag_action.hh"
#include "svg.hh"
#include "ui_connection_widget.hh"
#include "widget.hh"

namespace automat {

NextArg next_arg;

PaintDrawable& Argument::Icon() {
  static DrawableSkPath default_icon = [] {
    SkPath path = PathFromSVG(kNextShape);
    return DrawableSkPath(path);
  }();
  return default_icon;
}

bool Argument::IsOn(Location& here) const {
  if (auto* iface = StartInterface(*here.object)) {
    if (auto on_off = dynamic_cast<OnOff*>(iface)) {
      return on_off->IsOn();
    }
  }
  return false;
}

#pragma region New API

Vec2AndDir Argument::Start(ui::Widget& object_widget, ui::Widget& widget) const {
  auto* obj_widget_iface = dynamic_cast<Object::WidgetInterface*>(&object_widget);
  if (!obj_widget_iface) {
    return Vec2AndDir{};
  }
  auto pos_dir = obj_widget_iface->ArgStart(*this);
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
        if (!CanConnect(*here.object, *location->object)) {
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
    if (!CanConnect(*here.object, *other.object)) {
      return nullptr;
    }
    Vec<Vec2AndDir> to_points;
    other.WidgetForObject().ConnectionPositions(to_points);
    callback(other, to_points);
    return nullptr;
  });
}

Location* Argument::FindLocation(Location& here, const FindConfig& cfg) const {
  Location* result = nullptr;
  if (auto found = Find(*here.object)) {
    if (auto* obj = found.GetOwner<Object>()) {
      result = obj->MyLocation();
    }
  }
  if (result == nullptr && cfg.if_missing == IfMissing::CreateFromPrototype) {
    // Ask the current location for the prototype for this object.
    if (auto prototype = here.object->ArgPrototype(*this)) {
      if (auto machine = here.ParentAs<Machine>()) {
        Location& l = machine->Create(*prototype);
        result = &l;
        PositionAhead(here, *this, l);
        PositionBelow(l, here);
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

void Argument::InvalidateConnectionWidgets(Location& here) const {
  for (auto& w : ui::ConnectionWidgetRange(here, *this)) {
    w.WakeAnimation();
    if (w.state) {
      w.state->stabilized = false;
    }
  }
}

void NextArg::CanConnect(Named& start, Named& end, Status& status) const {
  if (!dynamic_cast<Runnable*>(&start)) {
    AppendErrorMessage(status) += "Next source must be a Runnable";
  }
  if (!dynamic_cast<Runnable*>(&end)) {
    AppendErrorMessage(status) += "Next target must be a Runnable";
  }
}

void NextArg::Connect(const NestedPtr<Named>& start, const NestedPtr<Named>& end) {
  Runnable* start_runnable = dynamic_cast<Runnable*>(start.Get());
  if (start_runnable == nullptr) return;
  start_runnable->next = end.DynamicCast<Runnable>();
  if (end) {
    if (Runnable* end_runnable = dynamic_cast<Runnable*>(end.Get())) {
      start_runnable->next = NestedWeakPtr<Runnable>(end.GetOwnerWeak(), end_runnable);
    }
  } else {
    start_runnable->next = {};
  }
}

NestedPtr<Named> NextArg::Find(Named& start) const {
  if (auto* runnable = dynamic_cast<Runnable*>(&start)) {
    return runnable->next.Lock();
  }
  return {};
}

}  // namespace automat
