#pragma once

#include <chrono>

namespace automaton {

struct Timer {
  using T = double;
  using duration = std::chrono::duration<T>;
  using clock = std::chrono::system_clock;
  using time_point = std::chrono::time_point<clock, duration>;
  time_point now = clock::now();
  time_point last = now;
  T d = 0; // delta from last frame
  void Tick() {
    now = clock::now();
    d = (now - last).count();
    last = now;
  }
};

extern Timer timer;

} // namespace automaton