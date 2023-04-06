#pragma once

#include <cmath>

#include "product_ptr.h"
#include "time.h"

namespace automaton::animation {

struct State {
  product_holder holder;
  operator product_holder &() { return holder; }
  time::Timer timer;
  operator time::Timer &() { return timer; }
};

struct DeltaFraction {
  float speed = 15;
  time::point last_tick;

  DeltaFraction(float speed = 15) : speed(speed), last_tick(time::now()) {}

  float Tick(time::Timer &timer) {
    float dt = (timer.now - last_tick).count();
    last_tick = timer.now;
    if (dt <= 0)
      return 0;
    return 1 - exp(-dt * speed);
  }
};

struct Approach {
  float value = 0;
  float target = 0;
  float speed = 15;
  float cap_min;
  float cap;
  time::point last_tick;

  Approach(float initial = 0, float cap_min = 0.01)
      : value(initial), target(initial), cap_min(cap_min), cap(cap_min),
        last_tick(time::now()) {}
  void Tick(time::Timer &timer) {
    float dt = (timer.now - last_tick).count();
    last_tick = timer.now;
    if (dt <= 0)
      return;
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

} // namespace automaton::animation
