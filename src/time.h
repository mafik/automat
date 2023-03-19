#pragma once

#include <chrono>

namespace automaton {

namespace time {

using T = double;
using duration = std::chrono::duration<T>;
using clock = std::chrono::system_clock;
using point = std::chrono::time_point<clock, duration>;

constexpr point kTimePointZero = {};

inline point now() { return clock::now(); }

struct Timer {
  point now = time::now();
  point last = now;
  T d = 0; // delta from last frame
  void Tick() {
    now = time::now();
    d = (now - last).count();
    last = now;
  }
};

extern Timer timer;

} // namespace time

} // namespace automaton