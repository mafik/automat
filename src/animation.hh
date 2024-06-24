#pragma once

#include <cmath>

#include "double_ptr.hh"
#include "time.hh"

namespace automat::animation {

// Holds data related to the device that displays an animation.
//
// Every frame the device should call `timer.Tick()` to update the timer.
//
// This struct should be kept alive for as long as the display device is active. On destruction it
// will also destroy temporary objects used for animation (see DoublePtrBase).
struct Context : maf::DoublePtrBase {
  // `timer` should be advanced once per frame on the device that displays the animation. Its `d`
  // field can be used by animated objects to animate their properties.
  time::Timer timer;
  operator time::Timer&() { return timer; }
};

struct DeltaFraction {
  float speed = 15;
  time::SystemPoint last_tick;

  // TODO: think about replacing `speed` with `half_life`.
  DeltaFraction(float speed = 15) : speed(speed), last_tick(time::SystemNow()) {}

  float Tick(time::Timer& timer) {
    float dt = (timer.now - last_tick).count();
    last_tick = timer.now;
    if (dt <= 0) return 0;
    return -expm1f(-dt * speed);  // equivalent to 1 - exp(-dt * speed);
  }
};

template <typename T>
struct Base {
  T value = {};
  T target = {};
};

struct Approach : Base<float> {
  float speed = 15;
  float cap_min;
  float cap;
  time::SystemPoint last_tick;

  Approach(float initial = 0, float cap_min = 0.01)
      : Base{.value = initial, .target = initial},
        cap_min(cap_min),
        cap(cap_min),
        last_tick(time::SystemNow()) {}
  void Tick(time::Timer& timer) {
    float dt = (timer.now - last_tick).count();
    last_tick = timer.now;
    if (dt <= 0) return;
    float delta = (target - value) * (-expm1f(-dt * speed));
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

template <typename T>
struct Spring : Base<T> {
  float acceleration = 100;
  T velocity = {};
  float friction = 10;
  time::SystemPoint last_tick;
  Spring() : last_tick(time::SystemNow()) {}
  void Tick(time::Timer& timer) {
    float dt = (timer.now - last_tick).count();
    last_tick = timer.now;
    if (dt <= 0) return;
    T delta = this->target - this->value;
    velocity += delta * acceleration * dt;
    velocity *= pow(0.5, dt * friction);
    this->value += velocity * dt;
  }
  operator T() const { return this->value; }
};

void WrapModulo(Base<float>& base, float range);

}  // namespace automat::animation
