#pragma once

namespace automat {

struct Location;

struct Connection {
  enum PointerBehavior { kFollowPointers, kTerminateHere };
  Location &from, &to;
  PointerBehavior pointer_behavior;
  Connection(Location& from, Location& to, PointerBehavior pointer_behavior);
  ~Connection();
};

}  // namespace automat