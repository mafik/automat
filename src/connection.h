#pragma once

namespace automaton {

struct Location;

struct Connection {
  enum PointerBehavior { kFollowPointers, kTerminateHere };
  Location &from, &to;
  PointerBehavior pointer_behavior;
  Connection(Location &from, Location &to, PointerBehavior pointer_behavior)
      : from(from), to(to), pointer_behavior(pointer_behavior) {}
};

} // namespace automaton