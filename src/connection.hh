#pragma once

#include <memory>

namespace automat {

struct Location;

struct ConnectionState {
  virtual ~ConnectionState() = default;
};

struct Connection {
  enum PointerBehavior { kFollowPointers, kTerminateHere };
  Location &from, &to;
  PointerBehavior pointer_behavior;
  std::unique_ptr<ConnectionState> state;
  Connection(Location& from, Location& to, PointerBehavior pointer_behavior)
      : from(from), to(to), pointer_behavior(pointer_behavior), state(nullptr) {}
};

}  // namespace automat