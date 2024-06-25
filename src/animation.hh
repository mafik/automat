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

  // Placeholder that can be used when no Window is available
  // TODO: replace with nullptr
  static Context kHeadless;
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

template <typename T = float>
struct Approach : Base<T> {
  float speed = 15;
  time::SystemPoint last_tick;

  Approach(T initial = {})
      : Base<T>{.value = initial, .target = initial}, last_tick(time::SystemNow()) {}
  void Tick(time::Timer& timer) {
    float dt = (timer.now - last_tick).count();
    last_tick = timer.now;
    if (dt <= 0) return;
    T delta = (this->target - this->value) * (-expm1f(-dt * speed));
    this->value += delta;
  }
  void Shift(float delta) {
    this->value += delta;
    this->target += delta;
  }
  float Remaining() const { return this->target - this->value; }
  operator float() const { return this->value; }
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
