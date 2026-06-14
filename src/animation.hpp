#pragma once
// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT

#include <cmath>

#include "math.hpp"
#include "sincos.hpp"
#include "time.hpp"

namespace automat::ui {
struct RootWidget;
}  // namespace automat::ui

namespace automat::animation {

// Result of one step of animation.
struct Progress {
  bool value_changed = false;
  bool settled = true;

  Progress& operator|=(Progress o) {
    value_changed |= o.value_changed;
    settled = settled && o.settled;
    return *this;
  }
};
inline Progress operator|(Progress a, Progress b) { return a |= b; }

// TODO: delete almost everything from this file (and replace with "LowLevel*Towards" functions)

template <typename T>
struct Base {
  T value = {};
  T target = {};
};

template <typename T = float>
struct Approach : Base<T> {
  float speed = 15;
  time::SteadyPoint last_tick;

  Approach(T initial = {})
      : Base<T>{.value = initial, .target = initial}, last_tick(time::SteadyNow()) {}
  Progress Tick(time::Timer& timer) {
    float dt = time::ToSeconds(timer.now - last_tick);
    last_tick = timer.now;
    if (this->value == this->target) return {.value_changed = false, .settled = true};
    if (dt <= 0) return {.value_changed = false, .settled = false};
    T before = this->value;
    this->value += (this->target - this->value) * (-expm1f(-dt * speed));
    if (this->value == before || fabsf(this->target - this->value) < 1e-6f) {
      this->value = this->target;
    }
    return {.value_changed = this->value != before, .settled = this->value == this->target};
  }
  void Shift(float delta) {
    this->value += delta;
    this->target += delta;
  }
  float Remaining() const { return this->target - this->value; }
  operator float() const { return this->value; }
};

inline Progress ExponentialApproach(float target, float delta_time, float e_time, float& value) {
  if (value == target) return {.value_changed = false, .settled = true};
  if (delta_time <= 0) return {.value_changed = false, .settled = false};
  float before = value;
  value += (target - value) * (-expm1f(-delta_time / e_time));
  if (value == before || fabsf(target - value) < 1e-6f) value = target;
  return {.value_changed = value != before, .settled = value == target};
}

inline Progress LinearApproach(float target, float delta_time, float speed, float& value) {
  // This can happen when animation was "woken" after this frame started rendering.
  // When another thread wakes the animation, then it's probably ok to set delta_time to 0.
  // When a widget's parents wake the child widgets then it's animation should probably already
  // progress. Otherwise we're adding a frame of latency. One workaround is for the parent to set
  // child.last_tick to its own value - so that the child animation starts earlier.
  // TODO: make the animation more responsive when a parent wakes a child widget
  if (delta_time < 0) delta_time = 0;
  if (value == target) return {.value_changed = false, .settled = true};
  float before = value;
  if (value < target) {
    value += delta_time * speed;
    if (value >= target) value = target;
  } else {
    value -= delta_time * speed;
    if (value <= target) value = target;
  }
  return {.value_changed = value != before, .settled = value == target};
}

// TODO: remove this and replace with `SpringV2`
template <typename T>
struct Spring : Base<T> {
  T velocity = {};
  time::Duration period = 100ms;     // how long does it take for one oscillation
  time::Duration half_life = 100ms;  // how long does it take for the amplitude to decrease by half
  time::SteadyPoint last_tick;
  Spring() : last_tick(time::SteadyNow()) {}
  Spring(T initial_value) : Spring() {
    this->value = initial_value;
    this->target = initial_value;
  }

  Progress TickComponent(float dt, float target, float& value, float& velocity) {
    float Q = 2 * M_PI / time::ToSeconds(period);
    float D = value - target;
    float V = velocity;
    float H = time::ToSeconds(half_life);

    float t;
    float amplitude;
    if (fabsf(D) > 1e-6f) {
      t = -atanf((D * kLog2e + V * H) / (D * H * Q)) / Q;
      amplitude = D / powf(2, -t / H) / cosf(t * Q);
    } else {
      if (fabsf(V) < 1e-6f) {
        bool value_changed = value != target || velocity != 0;
        value = target;
        velocity = 0;
        return {.value_changed = value_changed, .settled = true};
      }
      t = time::ToSeconds(period) / 4;
      amplitude = -velocity * powf(2.f, t / H) / Q;
    }
    float before = value;
    float t2 = t + dt;
    value = target + amplitude * cosf(t2 * Q) * powf(2, -t2 / H);
    velocity =
        (-(amplitude * kLog2e * cosf(t2 * Q)) / H - amplitude * Q * sinf(t2 * Q)) / powf(2, t2 / H);
    return {.value_changed = value != before, .settled = false};
  }

  template <typename P>
  Progress TickComponents(float dt) {
    return TickComponent(dt, this->target, this->value, this->velocity);
  }

  template <>
  Progress TickComponents<Vec2>(float dt) {
    auto x = TickComponent(dt, this->target.x, this->value.x, this->velocity.x);
    auto y = TickComponent(dt, this->target.y, this->value.y, this->velocity.y);
    return x | y;
  }

  Progress Tick(time::Timer& timer) {
    float dt = time::ToSeconds(timer.now - last_tick);
    last_tick = timer.now;
    if (half_life <= 0s || period <= 0s) return {};
    if (dt <= 0) return {.value_changed = false, .settled = false};

    return TickComponents<T>(dt);
  }
  operator T() const { return this->value; }
};

template <typename T>
struct SpringV2 {
  T value, velocity;

  SpringV2() : value{}, velocity{} {}
  SpringV2(T initial_value) : value(initial_value), velocity{} {}

  Progress SpringTowards(T target, float delta_time, float period_time, float half_time);

  Progress SineTowards(T target, float delta_time, float period_time);

  // If this spring is used to animate a ground-truth 'target' value through simple addition, then
  // this function can be used to update the target without causing a jump in the animation.
  //
  // Important: this only works if spring always animates towards zero!
  void SmoothTargetUpdate(T& target, T new_target) {
    value += target - new_target;
    target = new_target;
  }

  // Same as `SmoothTargetUpdate` but optimized for reduced latency of interactive updates (e.g.
  // dragging). If the spring is already locked on target, skips the animation.
  //
  // Important: this only works if spring always animates towards zero!
  void InteractiveTargetUpdate(T& target, T new_target) {
    if (velocity == T{} && value == T{}) {
      target = new_target;
    } else {
      SmoothTargetUpdate(target, new_target);
    }
  }

  operator T() const { return value; }
};

Progress LowLevelSpringTowards(float target, float delta_time, float period_time, float half_time,
                               float& value, float& velocity);

template <>
inline Progress SpringV2<float>::SpringTowards(float target, float delta_time, float period_time,
                                               float half_time) {
  return LowLevelSpringTowards(target, delta_time, period_time, half_time, value, velocity);
}

template <>
inline Progress SpringV2<Vec2>::SpringTowards(Vec2 target, float delta_time, float period_time,
                                              float half_time) {
  auto x = LowLevelSpringTowards(target.x, delta_time, period_time, half_time, value.x, velocity.x);
  auto y = LowLevelSpringTowards(target.y, delta_time, period_time, half_time, value.y, velocity.y);
  return x | y;
}

Progress LowLevelSineTowards(float target, float delta_time, float period_time, float& value,
                             float& velocity);

template <>
inline Progress SpringV2<float>::SineTowards(float target, float delta_time, float period_time) {
  return LowLevelSineTowards(target, delta_time, period_time, value, velocity);
}

template <>
inline Progress SpringV2<Vec2>::SineTowards(Vec2 target, float delta_time, float period_time) {
  auto x = LowLevelSineTowards(target.x, delta_time, period_time, value.x, velocity.x);
  auto y = LowLevelSineTowards(target.y, delta_time, period_time, value.y, velocity.y);
  return x | y;
}

template <>
inline Progress SpringV2<SinCos>::SineTowards(SinCos target, float delta_time, float period_time) {
  auto velocity_radians = velocity.ToRadians();
  float value_radians = 0;
  float target_radians = (target - value).ToRadians();
  auto progress =
      LowLevelSineTowards(target_radians, delta_time, period_time, value_radians, velocity_radians);
  value = value + SinCos::FromRadians(value_radians);
  velocity = SinCos::FromRadians(velocity_radians);
  return progress;
}

float SinInterp(float x, float x0, float y0, float x1, float y1);

// Add or subtract `RANGE` to `value` until it is within `target` +/- `RANGE/2`.
void WrapModulo(float& value, float target, float range);

}  // namespace automat::animation
