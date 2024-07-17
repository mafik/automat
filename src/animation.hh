#pragma once

#include <cmath>
#include <map>

#include "math.hh"
#include "time.hh"
#include "vec.hh"

using namespace std::chrono_literals;

namespace automat::animation {

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

  mutable std::map<void*, std::unique_ptr<PerDisplayValueBase>> per_display_values;
};

template <typename T>
struct PerDisplay {
  PerDisplay() = default;
  // forbid copy and move
  PerDisplay(const PerDisplay&) = delete;
  PerDisplay& operator=(const PerDisplay&) = delete;
  PerDisplay(PerDisplay&&) = delete;

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
    PerDisplay& ptr;
    size_t index = 0;
    iterator(PerDisplay& ptr) : ptr(ptr) { SkipMissing(); }
    void SkipMissing() {
      while (index < displays.size() &&
             !displays[index]->per_display_values.contains((void*)&ptr)) {
        ++index;
      }
    }
    T& operator*() { return *ptr.Find(*displays[index]); }
    iterator& operator++() {
      ++index;
      SkipMissing();
      return *this;
    }
    bool operator!=(const end_iterator&) { return index < displays.size(); }
  };

  iterator begin() { return iterator(*this); }
  end_iterator end() { return end_iterator{}; }
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
  T velocity = {};
  time::Duration period = 0.1s;     // how long does it take for one oscillation
  time::Duration half_life = 0.1s;  // how long does it take for the amplitude to decrease by half
  time::SteadyPoint last_tick;
  Spring() : last_tick(time::SteadyNow()) {}
  Spring(T initial_value) : Spring() {
    this->value = initial_value;
    this->target = initial_value;
  }

  void TickComponent(float dt, float target, float& value, float& velocity) {
    float Q = 2 * M_PI / period.count();
    float D = value - target;
    float V = velocity;
    float H = half_life.count();

    float t;
    float amplitude;
    if (fabsf(D) > 1e-6f) {
      t = -atanf((D * M_LOG2Ef + V * H) / (D * H * Q)) / Q;
      amplitude = D / powf(2, -t / H) / cosf(t * Q);
    } else {
      t = period.count() / 4;
      amplitude = -velocity * powf(2.f, t / H) / Q;
    }
    float t2 = t + dt;
    value = target + amplitude * cosf(t2 * Q) * powf(2, -t2 / H);
    velocity = (-(amplitude * M_LOG2Ef * cosf(t2 * Q)) / H - amplitude * Q * sinf(t2 * Q)) /
               powf(2, t2 / H);
  }

  template <typename P>
  void TickComponents(float dt) {
    TickComponent(dt, this->target, this->value, this->velocity);
  }

  template <>
  void TickComponents<Vec2>(float dt) {
    TickComponent(dt, this->target.x, this->value.x, this->velocity.x);
    TickComponent(dt, this->target.y, this->value.y, this->velocity.y);
  }

  void Tick(time::Timer& timer) {
    float dt = (timer.steady_now - last_tick).count();
    last_tick = timer.steady_now;
    if (dt <= 0) return;
    if (half_life.count() <= 0) return;
    if (period.count() <= 0) return;

    TickComponents<T>(dt);
  }
  operator T() const { return this->value; }
};

void WrapModulo(Base<float>& base, float range);

}  // namespace automat::animation
