// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "argument.hh"

#include <cmath>

#include "base.hh"
#include "drag_action.hh"
#include "root_widget.hh"
#include "svg.hh"
#include "ui_connection_widget.hh"
#include "ui_shape_widget.hh"
#include "widget.hh"

namespace automat {

std::unique_ptr<ui::Widget> Argument::MakeIcon(ui::Widget* parent) const {
  return table->make_icon(*this, parent);
}

Object* Argument::ObjectOrNull() const {
  if (auto found = Find()) {
    if (auto* o = found.Owner<Object>()) {
      return o;
    }
  }
  return nullptr;
}

Object& Argument::ObjectOrMake() const {
  if (auto* o = ObjectOrNull()) {
    return *o;
  }
  auto proto = table->prototype();
  Location* start_loc = object_ptr->here;
  auto board = start_loc->ParentAs<Board>();
  auto& loc = board->Create(*proto);

  PositionAhead(*start_loc, *table, loc);
  PositionBelow(loc, *start_loc);
  Connect(*loc.object);

  ui::root_widget->WakeAnimation();
  return *loc.object;
}

std::unique_ptr<Argument::Toy> Argument::MakeToy(ui::Widget* parent) {
  return std::make_unique<ui::ConnectionWidget>(parent, *object_ptr, *table);
}

std::unique_ptr<ui::Widget> Argument::Table::DefaultMakeIcon(Argument, ui::Widget* parent) {
  return ui::MakeShapeWidget(parent, kNextShape, "#ffffff"_color);
}

}  // namespace automat
