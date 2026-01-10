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

  Vec2 center = here.WidgetForObject().ArgStart(*this, root_machine.get()).pos;
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

Object* Argument::FindObject(Location& here, const FindConfig& cfg) const {
  Object* result = nullptr;
  if (auto found = Find(*here.object)) {
    if (auto* obj = found.Owner<Object>()) {
      result = obj;
    }
  }
  if (result == nullptr && cfg.if_missing == IfMissing::CreateFromPrototype) {
    // Ask the argument for the prototype for this object.
    if (auto prototype = Prototype()) {
      if (auto machine = here.ParentAs<Machine>()) {
        Location& l = machine->Create(*prototype);
        result = l.object.get();
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

void NextArg::CanConnect(Object& start, Part& end, Status& status) const {
  if (!dynamic_cast<Runnable*>(&start)) {
    AppendErrorMessage(status) += "Next source must be a Runnable";
  }
  if (!dynamic_cast<Runnable*>(&end)) {
    AppendErrorMessage(status) += "Next target must be a Runnable";
  }
}

void NextArg::Connect(Object& start, const NestedPtr<Part>& end) {
  Runnable* start_runnable = dynamic_cast<Runnable*>(&start);
  if (start_runnable == nullptr) return;
  if (end) {
    if (Runnable* end_runnable = dynamic_cast<Runnable*>(end.Get())) {
      start_runnable->next = NestedWeakPtr<Runnable>(end.GetOwnerWeak(), end_runnable);
    }
  } else {
    start_runnable->next = {};
  }
}

NestedPtr<Part> NextArg::Find(Object& start) const {
  if (auto* runnable = dynamic_cast<Runnable*>(&start)) {
    return runnable->next.Lock();
  }
  return {};
}

}  // namespace automat
