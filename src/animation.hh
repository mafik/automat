// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <cmath>

#include "math.hh"
#include "time.hh"

namespace automat::ui {
struct RootWidget;
}  // namespace automat::ui

namespace automat::animation {

enum class Phase : bool {
  Finished = false,  // default value, when initialized with {}
  Animating = true,
};

using enum Phase;

inline Str ToStr(Phase p) { return p == Animating ? "Animating" : "Finished"; }

inline Phase operator||(Phase a, Phase b) { return Phase(bool(a) || bool(b)); }
inline Phase& operator|=(Phase& a, Phase b) { return a = a || b; }

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
  animation::Phase Tick(time::Timer& timer) {
    float dt = time::ToSeconds(timer.now - last_tick);
    last_tick = timer.now;
    if (dt <= 0) return animation::Finished;
    T delta = (this->target - this->value) * (-expm1f(-dt * speed));
    if (fabsf(delta) < 1e-6f) {
      this->value = this->target;
      return animation::Finished;
    } else {
      this->value += delta;
      return animation::Animating;
    }
  }
  void Shift(float delta) {
    this->value += delta;
    this->target += delta;
  }
  float Remaining() const { return this->target - this->value; }
  operator float() const { return this->value; }
};

inline Phase ExponentialApproach(float target, float delta_time, float e_time, float& value) {
  if (delta_time <= 0) return Finished;
  float delta = target - value;
  if (delta == 0) return Finished;
  if (fabsf(delta) < 1e-6f) {
    value = target;
  } else {
    float old_value = value;
    value += delta * (-expm1f(-delta_time / e_time));
    if (old_value == value) {
      value = target;
    } else if (fabsf(target - value) < 1e-6f) {
      value = target;
    }
  }
  return Animating;
}

inline Phase LinearApproach(float target, float delta_time, float speed, float& value) {
  if (delta_time <= 0) return Finished;
  if (value < target) {
    value += delta_time * speed;
    if (value >= target) {
      value = target;
      return Finished;
    }
  } else if (value > target) {
    value -= delta_time * speed;
    if (value < target) {
      value = target;
      return Finished;
    }
  } else {
    return Finished;
  }
  return Animating;
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

  Phase TickComponent(float dt, float target, float& value, float& velocity) {
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
        value = target;
        velocity = 0;
        return Finished;
      }
      t = time::ToSeconds(period) / 4;
      amplitude = -velocity * powf(2.f, t / H) / Q;
    }
    float t2 = t + dt;
    value = target + amplitude * cosf(t2 * Q) * powf(2, -t2 / H);
    velocity =
        (-(amplitude * kLog2e * cosf(t2 * Q)) / H - amplitude * Q * sinf(t2 * Q)) / powf(2, t2 / H);
    return Animating;
  }

  template <typename P>
  Phase TickComponents(float dt) {
    return TickComponent(dt, this->target, this->value, this->velocity);
  }

  template <>
  Phase TickComponents<Vec2>(float dt) {
    auto x = TickComponent(dt, this->target.x, this->value.x, this->velocity.x);
    auto y = TickComponent(dt, this->target.y, this->value.y, this->velocity.y);
    return x || y;
  }

  Phase Tick(time::Timer& timer) {
    float dt = time::ToSeconds(timer.now - last_tick);
    last_tick = timer.now;
    if (dt <= 0) return Finished;
    if (half_life <= 0s) return Finished;
    if (period <= 0s) return Finished;

    return TickComponents<T>(dt);
  }
  operator T() const { return this->value; }
};

template <typename T>
struct SpringV2 {
  T value, velocity;

  SpringV2() : value{}, velocity{} {}
  SpringV2(T initial_value) : value(initial_value), velocity{} {}

  Phase SpringTowards(T target, float delta_time, float period_time, float half_time);

  Phase SineTowards(T target, float delta_time, float period_time);

  operator T() const { return value; }
};

Phase LowLevelSpringTowards(float target, float delta_time, float period_time, float half_time,
                            float& value, float& velocity);

template <>
inline Phase SpringV2<float>::SpringTowards(float target, float delta_time, float period_time,
                                            float half_time) {
  return LowLevelSpringTowards(target, delta_time, period_time, half_time, value, velocity);
}

template <>
inline Phase SpringV2<Vec2>::SpringTowards(Vec2 target, float delta_time, float period_time,
                                           float half_time) {
  auto x = LowLevelSpringTowards(target.x, delta_time, period_time, half_time, value.x, velocity.x);
  auto y = LowLevelSpringTowards(target.y, delta_time, period_time, half_time, value.y, velocity.y);
  return x || y;
}

Phase LowLevelSineTowards(float target, float delta_time, float period_time, float& value,
                          float& velocity);

template <>
inline Phase SpringV2<float>::SineTowards(float target, float delta_time, float period_time) {
  return LowLevelSineTowards(target, delta_time, period_time, value, velocity);
}

template <>
inline Phase SpringV2<Vec2>::SineTowards(Vec2 target, float delta_time, float period_time) {
  auto x = LowLevelSineTowards(target.x, delta_time, period_time, value.x, velocity.x);
  auto y = LowLevelSineTowards(target.y, delta_time, period_time, value.y, velocity.y);
  return x || y;
}

float SinInterp(float x, float x0, float y0, float x1, float y1);

// Add or subtract `RANGE` to `value` until it is within `target` +/- `RANGE/2`.
void WrapModulo(float& value, float target, float range);

}  // namespace automat::animation
