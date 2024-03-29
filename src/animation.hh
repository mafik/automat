#pragma once

#include <cmath>

#include "product_ptr.hh"
#include "time.hh"

namespace automat::animation {

// Holds data related to the device that displays an animation.
//
// Every frame the device should call `timer.Tick()` to update the timer.
//
// This struct should be kept alive for as long as the display device is active. On destruction it
// will also destroy temporary objects used for animation.
struct Context {
  // `holder` can be used with `product_ptr` to create temporary objects which will be destroyed
  // alongside Context (or `product_ptr` - whichever comes first).
  product_holder holder;
  operator product_holder&() { return holder; }
  // `timer` should be advanced once per frame on the device that displays the animation. Its `d`
  // field can be used by animated objects to animate their properties.
  time::Timer timer;
  operator time::Timer&() { return timer; }
};

struct DeltaFraction {
  float speed = 15;
  time::point last_tick;

  DeltaFraction(float speed = 15) : speed(speed), last_tick(time::now()) {}

  float Tick(time::Timer& timer) {
    float dt = (timer.now - last_tick).count();
    last_tick = timer.now;
    if (dt <= 0) return 0;
    return 1 - exp(-dt * speed);
  }
};

struct Base {
  float value = 0;
  float target = 0;
};

struct Approach : Base {
  float speed = 15;
  float cap_min;
  float cap;
  time::point last_tick;

  Approach(float initial = 0, float cap_min = 0.01)
      : Base{.value = initial, .target = initial},
        cap_min(cap_min),
        cap(cap_min),
        last_tick(time::now()) {}
  void Tick(time::Timer& timer) {
    float dt = (timer.now - last_tick).count();
    last_tick = timer.now;
    if (dt <= 0) return;
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

struct Spring : Base {
  float acceleration = 100;
  float velocity = 0;
  float friction = 10;
  time::point last_tick;
  Spring() : last_tick(time::now()) {}
  void Tick(time::Timer& timer) {
    float dt = (timer.now - last_tick).count();
    last_tick = timer.now;
    if (dt <= 0) return;
    float delta = target - value;
    velocity += delta * acceleration * dt;
    velocity *= pow(0.5, dt * friction);
    value += velocity * dt;
  }
  operator float() const { return value; }
};

void WrapModulo(Base& base, float range);

}  // namespace automat::animation
