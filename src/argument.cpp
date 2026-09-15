// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "argument.hpp"

#include <cmath>

#include "base.hpp"
#include "board.hpp"
#include "drag_action.hpp"
#include "svg.hpp"
#include "sync.hpp"
#include "ui_connection_widget.hpp"
#include "ui_shape_widget.hpp"
#include "widget.hpp"

namespace automat {

Object* Argument::ObjectOrNull() const { return Find().object_ptr; }

Object& Argument::ObjectOrMake() const {
  if (auto* o = ObjectOrNull()) {
    return *o;
  }
  auto proto = table->prototype();
  auto start_loc = object_ptr->HomeLocation();
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
  switch (table->style) {
    case Style::Arrow:
      return std::make_unique<ui::ConnectionWidget>(parent, *object_ptr, *table);
    case Style::Cable:
      return std::make_unique<ui::CableWidget>(parent, *object_ptr, *table);
    case Style::RoutedCable:
      return std::make_unique<ui::RoutedCableWidget>(parent, *object_ptr, *table);
    case Style::Spotlight:
      return std::make_unique<ui::SpotlightWidget>(parent, *object_ptr, *table);
    case Style::Invisible:
      return std::make_unique<ui::InvisibleWidget>(parent, *object_ptr, *table);
    case Style::Stream:
      return std::make_unique<ui::StreamPipeWidget>(parent, *object_ptr, *table);
    case Style::Belt:
      return std::make_unique<SyncBelt>(parent, *object_ptr, *cast<Syncable::Table>(table_ptr));
  }
  return nullptr;
}

std::unique_ptr<Action> Argument::Table::DefaultActivate(Interface, ui::Pointer& pointer,
                                                         automat::Toy* toy) {
  auto* widget = dynamic_cast<ui::ConnectionWidget*>(toy);
  return widget ? std::make_unique<ui::DragConnectionAction>(pointer, *widget) : nullptr;
}

std::unique_ptr<ui::Widget> Argument::Table::DefaultMakeIcon(Interface, ui::Widget* parent) {
  return ui::MakeShapeWidget(parent, PathFromSVG(kNextShape), "#ffffff"_color);
}

}  // namespace automat
