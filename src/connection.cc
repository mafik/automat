// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "connection.hh"

#include "location.hh"

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
}

Connection::Connection(Argument& arg, Location& from, Location& to,
                       PointerBehavior pointer_behavior)
    : argument(arg), from(from), to(to), pointer_behavior(pointer_behavior) {}
}  // namespace automat