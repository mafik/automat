export module time;

import <chrono>;

namespace automaton {

using time_point = std::chrono::steady_clock::time_point;
using duration = std::chrono::steady_clock::duration;

time_point now;
time_point last_update;
duration update_delta;

void UpdateNow() {
  now = std::chrono::steady_clock::now();
}

void UpdateDelta() {
  last_update = now;
  UpdateNow();
  update_delta = now - last_update;
}

void Init() {
  UpdateNow();
  last_update = now;
  update_delta = now - last_update;
}

}