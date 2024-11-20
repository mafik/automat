// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "connection.hh"

#include "gui_connection_widget.hh"
#include "location.hh"
#include "window.hh"

using namespace automat::gui;

namespace automat {
Connection::~Connection() {
  from.object->ConnectionRemoved(from, *this);
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
  if (window) {
    for (int i = 0; i < window->connection_widgets.size(); ++i) {
      auto& widget = *window->connection_widgets[i];
      if (&widget.from == &from && &widget.arg == &argument) {
        widget.InvalidateDrawCache();
      }
    }
  }
}

Connection::Connection(Argument& arg, Location& from, Location& to,
                       PointerBehavior pointer_behavior)
    : argument(arg), from(from), to(to), pointer_behavior(pointer_behavior) {}
}  // namespace automat