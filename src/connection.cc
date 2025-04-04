// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "connection.hh"

#include "gui_connection_widget.hh"
#include "location.hh"
#include "root_widget.hh"

using namespace automat::gui;

namespace automat {
Connection::~Connection() {
  auto [begin, end] = from.outgoing.equal_range(this);
  for (auto it = begin; it != end; ++it) {
    if (*it == this) {
      from.outgoing.erase(it);
      break;
    }
  }
  auto [begin2, end2] = to.incoming.equal_range(this);
  for (auto it = begin2; it != end2; ++it) {
    if (*it == this) {
      to.incoming.erase(it);
      break;
    }
  }
  if (root_widget) {
    for (int i = 0; i < root_widget->connection_widgets.size(); ++i) {
      auto& widget = *root_widget->connection_widgets[i];
      if (&widget.from == &from && &widget.arg == &argument) {
        widget.WakeAnimation();
      }
    }
  }
  if (from.object) {
    from.object->ConnectionRemoved(from, *this);
  }
}

Connection::Connection(Argument& arg, Location& from, Location& to,
                       PointerBehavior pointer_behavior)
    : argument(arg), from(from), to(to), pointer_behavior(pointer_behavior) {}
}  // namespace automat