// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <chrono>
#include <cmath>

namespace automat::time {

using T = double;
using Duration = std::chrono::duration<T>;
using SystemClock = std::chrono::system_clock;
using SteadyClock = std::chrono::steady_clock;
using SystemPoint = std::chrono::time_point<SystemClock, Duration>;
using SteadyPoint = std::chrono::time_point<SteadyClock, Duration>;

constexpr SystemPoint kZero = {};
constexpr SteadyPoint kZeroSteady = {};

inline SystemPoint SystemNow() { return SystemClock::now(); }
inline SteadyPoint SteadyNow() { return SteadyClock::now(); }

// Sawtooth wave [0, 1).
template <auto Period>
T SteadySaw() {
  T t = SteadyNow().time_since_epoch().count();
  return std::fmod(t / Period, 1);
}

SystemPoint SystemFromSteady(SteadyPoint steady);
SteadyPoint SteadyFromSystem(SystemPoint system);

struct Timer {
  SteadyPoint now = time::SteadyNow();
  SteadyPoint last = now;
  T d = 0;  // delta from last frame
  T NowSeconds() const { return (now.time_since_epoch()).count(); }
  void Tick() {
    last = now;
    now = time::SteadyNow();
    d = (now - last).count();
  }
};

}  // namespace automat::time