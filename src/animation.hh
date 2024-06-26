#pragma once

#include <cmath>
#include <map>

#include "time.hh"
#include "vec.hh"

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
