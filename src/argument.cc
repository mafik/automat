// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "argument.hh"

#include <cmath>

#include "base.hh"
#include "drag_action.hh"
#include "log.hh"
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

Object* Argument::ObjectOrNull(Object& start) const {
  if (auto found = Find(start)) {
    if (auto* obj = found.Owner<Object>()) {
      return obj;
    }
  }
  return nullptr;
}

Object& Argument::ObjectOrMake(Object& start) const {
  if (auto* obj = ObjectOrNull(start)) {
    return *obj;
  }
  // Ask the argument for the prototype for this object.
  auto prototype = Prototype();
  Location* here = start.MyLocation();
  auto machine = here->ParentAs<Machine>();
  Location& l = machine->Create(*prototype);
  PositionAhead(*here, *this, l);
  PositionBelow(l, *here);
  AnimateGrowFrom(*here,
                  l);  // this must go before UpdateAutoconnectArgs because of animation_state
  l.UpdateAutoconnectArgs();
  l.WakeAnimation();
  return *l.object;
}

std::unique_ptr<Toy> ArgumentOf::MakeToy(ui::Widget* parent) {
  return std::make_unique<ui::ConnectionWidget>(parent, object, arg);
}

void NextArg::CanConnect(Object& start, Part& end, Status& status) const {
  if (!dynamic_cast<SignalNext*>(&start)) {
    AppendErrorMessage(status) += "Next source must be a Runnable";
  }
  if (!dynamic_cast<Runnable*>(&end)) {
    AppendErrorMessage(status) += "Next target must be a Runnable";
  }
}

void NextArg::Connect(Object& start, const NestedPtr<Part>& end) {
  SignalNext* start_signal = dynamic_cast<SignalNext*>(&start);
  if (start_signal == nullptr) return;
  if (end) {
    if (Runnable* end_runnable = dynamic_cast<Runnable*>(end.Get())) {
      start_signal->next = NestedWeakPtr<Runnable>(end.GetOwnerWeak(), end_runnable);
    }
  } else {
    start_signal->next = {};
  }
}

NestedPtr<Part> NextArg::Find(const Object& start) const {
  if (auto* start_signal = dynamic_cast<const SignalNext*>(&start)) {
    return start_signal->next.Lock();
  } else {
    ERROR_ONCE << start.Name()
               << " is not a SignalNext - and can't be used as a source for NextArg";
  }
  return {};
}

}  // namespace automat
