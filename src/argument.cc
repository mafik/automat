// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "argument.hh"

#include <cmath>

#include "base.hh"
#include "casting.hh"
#include "drag_action.hh"
#include "log.hh"
#include "root_widget.hh"
#include "svg.hh"
#include "ui_connection_widget.hh"
#include "ui_shape_widget.hh"
#include "widget.hh"

namespace automat {

std::unique_ptr<ui::Widget> Argument::MakeIcon(ui::Widget* parent) const {
  return make_icon(*this, parent);
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
  auto proto = prototype();
  Location* start_loc = start.here;
  auto board = start_loc->ParentAs<Board>();
  auto& loc = board->Create(*proto);

  PositionAhead(*start_loc, *this, loc);
  PositionBelow(loc, *start_loc);
  Connect(start, *loc.object);

  ui::root_widget->WakeAnimation();
  return *loc.object;
}

std::unique_ptr<ArgumentOf::Toy> ArgumentOf::MakeToy(ui::Widget* parent) {
  return std::make_unique<ui::ConnectionWidget>(parent, object, arg);
}

// --- NextArg implementation ---

NextArg::NextArg(StrView name) : Argument(name, Interface::kNextArg) {
  style = Style::Cable;
  can_connect = [](const Argument&, Object& start, Object& end_obj, Interface* end_iface,
                   Status& status) {
    if (!dyn_cast_if_present<Runnable>(end_iface)) {
      AppendErrorMessage(status) += "Next target must be a Runnable";
    }
  };
  on_connect = [](const Argument& arg, Object& start, Object* end_obj, Interface* end_iface) {
    auto& next_arg = static_cast<const NextArg&>(arg);
    auto& state = next_arg.get_next_state(start);
    if (end_obj) {
      if (Runnable* end_runnable = dyn_cast_if_present<Runnable>(end_iface)) {
        state.next = NestedWeakPtr<Runnable>(end_obj->AcquireWeakPtr(), end_runnable);
      }
    } else {
      state.next = {};
    }
  };
  find = [](const Argument& arg, const Object& start) -> NestedPtr<Interface> {
    auto& next_arg = static_cast<const NextArg&>(arg);
    auto& state = next_arg.get_next_state(const_cast<Object&>(start));
    return state.next.Lock();
  };
  make_icon = [](const Argument&, ui::Widget* parent) {
    return ui::MakeShapeWidget(parent, kNextShape, "#ffffff"_color);
  };
}

}  // namespace automat
