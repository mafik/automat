// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <functional>
#include <type_traits>

namespace automat {

struct Location;
struct Argument;

struct Connection {
  enum PointerBehavior { kFollowPointers, kTerminateHere };
  Argument& argument;
  Location &from, &to;
  PointerBehavior pointer_behavior;
  Connection(Argument&, Location& from, Location& to, PointerBehavior);
  ~Connection();
};

struct ConnectionHasher {
  using is_transparent = std::true_type;
  size_t operator()(const Connection* c) const {
    return std::hash<const Argument*>{}(&c->argument);
  }
  size_t operator()(const Argument* a) const { return std::hash<const Argument*>{}(a); }
};

struct ConnectionEqual {
  using is_transparent = std::true_type;
  bool operator()(const Connection* a, const Connection* b) const {
    return &a->argument == &b->argument;
  }
  bool operator()(const Connection* a, const Argument* b) const { return &a->argument == b; }
  bool operator()(const Argument* a, const Connection* b) const { return a == &b->argument; }
};

}  // namespace automat