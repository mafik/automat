// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "argument.hpp"

#include <cmath>

#include "base.hpp"
#include "drag_action.hpp"
#include "svg.hpp"
#include "ui_connection_widget.hpp"
#include "ui_shape_widget.hpp"
#include "widget.hpp"

namespace automat {

std::unique_ptr<ui::Widget> Argument::MakeIcon(ui::Widget* parent) const {
  return table->make_icon(*this, parent);
}

Object* Argument::ObjectOrNull() const {
  auto found = Find();
  if (auto* o = found.Owner<Object>()) {
    return o;
  }
  return nullptr;
}

Object& Argument::ObjectOrMake() const {
  if (auto* o = ObjectOrNull()) {
    return *o;
  }
  auto proto = table->prototype();
  Location* start_loc = object_ptr->MyLocation();
  Ptr<Board> board = start_loc ? start_loc->LockBoard() : nullptr;
  if (!board) {
    board = DefaultBoard().AcquirePtr();
  }
  auto& loc = board->Create(*proto);

  if (start_loc) {
    loc.placement = Location::PlaceAhead{start_loc->AcquireWeakPtr(), table};
  }
  Connect(*loc.object);

  return *loc.object;
}

std::unique_ptr<Argument::Toy> Argument::MakeToy(ui::Widget* parent) {
  return std::make_unique<ui::ConnectionWidget>(parent, *object_ptr, *table);
}

std::unique_ptr<ui::Widget> Argument::Table::DefaultMakeIcon(Argument, ui::Widget* parent) {
  return ui::MakeShapeWidget(parent, PathFromSVG(kNextShape), "#ffffff"_color);
}

}  // namespace automat
