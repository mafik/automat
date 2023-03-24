#pragma once

#include <cmath>

namespace automaton {
  
struct AnimatedApproach {
  float value = 0;
  float target = 0;
  float speed = 15;
  float cap_min;
  float cap;
  AnimatedApproach(float initial, float cap_min = 0.01)
      : value(initial), target(initial), cap_min(cap_min), cap(cap_min) {}
  void Tick(float dt) {
    float delta = (target - value) * (1 - exp(-dt * speed));
    float delta_abs = fabs(delta);
    if (delta_abs > cap * dt) {
      value += cap * dt * (delta > 0 ? 1 : -1);
      cap = std::min(delta_abs / dt, 2 * cap);
    } else {
      value += delta;
      cap = std::max(delta_abs / dt, cap_min);
    }
  }
  void Shift(float delta) {
    value += delta;
    target += delta;
  }
  float Remaining() const { return target - value; }
  operator float() const { return value; }
};

} // namespace automaton