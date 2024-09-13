#pragma once

#include <cmath>
#include <map>

#include "math.hh"
#include "time.hh"
#include "vec.hh"

using namespace std::chrono_literals;

namespace automat::gui {
struct Window;
}  // namespace automat::gui

namespace automat::animation {

enum class Phase : bool {
  Finished = false,  // default value, when initialized with {}
  Animating = true,
};

using enum Phase;

inline Phase operator||(Phase a, Phase b) { return Phase(bool(a) || bool(b)); }
inline Phase& operator|=(Phase& a, Phase b) { return a = a || b; }

struct PerDisplayValueBase {
  const void* owner;
  PerDisplayValueBase(const void* owner) : owner(owner) {}
  virtual ~PerDisplayValueBase() = default;
};

template <typename T>
struct PerDisplayValue : PerDisplayValueBase {
  T value;
  PerDisplayValue(const void* owner) : PerDisplayValueBase(owner), value{} {}
  ~PerDisplayValue() override = default;
};

struct Display;

extern maf::Vec<Display*> displays;

// Holds data related to the device that displays an animation.
//
// Every frame the device should call `timer.Tick()` to update the timer.
//
// This struct should be kept alive for as long as the display device is active. On destruction it
// will also destroy temporary objects used for animation.
struct Display {
  Display() { displays.push_back(this); }
  // forbid copy and move
  Display(const Display&) = delete;
  Display& operator=(const Display&) = delete;
  Display(Display&&) = delete;

  ~Display() { displays.erase(std::find(displays.begin(), displays.end(), this)); }

  // `timer` should be advanced once per frame on the device that displays the animation. Its `d`
  // field can be used by animated objects to animate their properties.
  time::Timer timer;
  operator time::Timer&() { return timer; }

  gui::Window* window = nullptr;

  mutable std::map<void*, std::unique_ptr<PerDisplayValueBase>> per_display_values;
};

template <typename T>
struct PerDisplay {
  PerDisplay() = default;

  // copy
  PerDisplay(const PerDisplay& orig) {
    *this = orig;  // defer to operator=
  }
  PerDisplay& operator=(const PerDisplay& orig) {
    for (auto* display : displays) {
      if (auto it = display->per_display_values.find((void*)&orig);
          it != display->per_display_values.end()) {
        auto copy = std::make_unique<PerDisplayValue<T>>(this);
        copy->value = ((PerDisplayValue<T>*)it->second.get())->value;
        display->per_display_values.emplace(this, std::move(copy));
      }
    }
    return *this;
  }

  // moving is slow but ok
  PerDisplay(PerDisplay&& orig) {
    for (auto* display : displays) {
      if (auto it = display->per_display_values.find((void*)&orig);
          it != display->per_display_values.end()) {
        auto value = std::move(it->second);
        value->owner = this;
        display->per_display_values.erase(it);
        display->per_display_values.emplace(this, std::move(value));
      }
    }
  }
  PerDisplay& operator=(const PerDisplay&& orig) {
    for (auto* display : displays) {
      if (auto it = display->per_display_values.find((void*)&orig);
          it != display->per_display_values.end()) {
        auto value = std::move(it->second);
        value->owner = this;
        display->per_display_values.erase(it);
        display->per_display_values.emplace(this, std::move(value));
      }
    }
    return *this;
  }

  ~PerDisplay() {
    for (auto* display : displays) {
      if (auto it = display->per_display_values.find(this);
          it != display->per_display_values.end()) {
        display->per_display_values.erase(it);
      }
    }
  }

  T* Find(const Display& d) const {
    if (auto it = d.per_display_values.find((void*)this); it != d.per_display_values.end()) {
      return &static_cast<PerDisplayValue<T>*>(it->second.get())->value;
    }
    return nullptr;
  }

  T& operator[](const Display& d) const {
    if (auto* val = Find(d)) {
      return *val;
    }
    auto [it, present] = d.per_display_values.emplace((void*)this, new PerDisplayValue<T>(this));
    return static_cast<PerDisplayValue<T>*>(it->second.get())->value;
  }

  struct end_iterator {};

  struct iterator {
    const PerDisplay& ptr;
    size_t index = 0;
    iterator(const PerDisplay& ptr) : ptr(ptr) { SkipMissing(); }
    void SkipMissing() {
      while (index < displays.size() &&
             !displays[index]->per_display_values.contains((void*)&ptr)) {
        ++index;
      }
    }
    Display* Display() { return displays[index]; }
    T& operator*() { return *ptr.Find(*displays[index]); }
    iterator& operator++() {
      ++index;
      SkipMissing();
      return *this;
    }
    bool operator!=(const end_iterator&) { return index < displays.size(); }
  };

  iterator begin() const { return iterator(*this); }
  end_iterator end() const { return end_iterator{}; }
};

// TODO: delete almost everything from this file (and replace with "LowLevel*Towards" functions)
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
  animation::Phase Tick(time::Timer& timer) {
    float dt = (timer.now - last_tick).count();
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
  if (fabsf(delta) < 1e-6f) {
    value = target;
    return Finished;
  } else {
    float old_value = value;
    value += delta * (-expm1f(-delta_time / e_time));
    if (old_value == value) return Finished;
    return Animating;
  }
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
  time::Duration period = 0.1s;     // how long does it take for one oscillation
  time::Duration half_life = 0.1s;  // how long does it take for the amplitude to decrease by half
  time::SteadyPoint last_tick;
  Spring() : last_tick(time::SteadyNow()) {}
  Spring(T initial_value) : Spring() {
    this->value = initial_value;
    this->target = initial_value;
  }

  Phase TickComponent(float dt, float target, float& value, float& velocity) {
    float Q = 2 * M_PI / period.count();
    float D = value - target;
    float V = velocity;
    float H = half_life.count();

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
      t = period.count() / 4;
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
    float dt = (timer.steady_now - last_tick).count();
    last_tick = timer.steady_now;
    if (dt <= 0) return Finished;
    if (half_life.count() <= 0) return Finished;
    if (period.count() <= 0) return Finished;

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
template <float RANGE>
void WrapModulo(float& value, float target) {
  auto delta = value - target;
  // This could probably be simplified some more
  value -= truncf(copysignf(fabsf(delta) + RANGE / 2, delta) / RANGE) * RANGE;
}

}  // namespace automat::animation
