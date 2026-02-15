// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "argument.hh"

#include <cmath>

#include "base.hh"
#include "drag_action.hh"
#include "log.hh"
#include "root_widget.hh"
#include "svg.hh"
#include "ui_connection_widget.hh"
#include "ui_shape_widget.hh"
#include "widget.hh"

namespace automat {

NextArg next_arg;

std::unique_ptr<ui::Widget> Argument::MakeIcon(ui::Widget* parent) {
  return ui::MakeShapeWidget(parent, kNextShape, "#ffffff"_color);
}

Object* Argument::ObjectOrNull(Object& start) const {
  if (auto found = Find(start)) {
    if (auto* obj = found.Owner<Object>()) {
      return obj;
    }
  }
  return nullptr;
}

Object& Argument::ObjectOrMake(Object& start) {
  if (auto* obj = ObjectOrNull(start)) {
    return *obj;
  }
  auto proto = Prototype();
  Location* start_loc = start.here;
  auto board = start_loc->ParentAs<Board>();
  auto& loc = board->Create(*proto);

  PositionAhead(*start_loc, *this, loc);
  PositionBelow(loc, *start_loc);
  Connect(start, NestedPtr(loc.object, &Object::toplevel_interface));

  ui::root_widget->WakeAnimation();
  return *loc.object;
}

std::unique_ptr<ArgumentOf::Toy> ArgumentOf::MakeToy(ui::Widget* parent) {
  return std::make_unique<ui::ConnectionWidget>(parent, object, arg);
}

void NextArg::CanConnect(Object& start, Object& end_obj, Interface& end_iface,
                         Status& status) const {
  if (!start.AsSignalNext()) {
    AppendErrorMessage(status) += "Next source must be a Runnable";
  }
  if (!dynamic_cast<Runnable*>(&end_iface)) {
    AppendErrorMessage(status) += "Next target must be a Runnable";
  }
}

void NextArg::OnConnect(Object& start, const NestedPtr<Interface>& end) {
  SignalNext* start_signal = start.AsSignalNext();
  if (start_signal == nullptr) return;
  if (end) {
    if (Runnable* end_runnable = dynamic_cast<Runnable*>(end.Get())) {
      start_signal->next = NestedWeakPtr<Runnable>(end.GetOwnerWeak(), end_runnable);
    }
  } else {
    start_signal->next = {};
  }
}

NestedPtr<Interface> NextArg::Find(const Object& start) const {
  if (auto* start_signal = const_cast<Object&>(start).AsSignalNext()) {
    return start_signal->next.Lock();
  } else {
    ERROR_ONCE << start.Name()
               << " is not a SignalNext - and can't be used as a source for NextArg";
  }
  return {};
}

}  // namespace automat
