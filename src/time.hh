// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <chrono>

namespace automat::time {

using T = double;
using Duration = std::chrono::duration<T>;
using SystemClock = std::chrono::system_clock;
using SteadyClock = std::chrono::steady_clock;
using SystemPoint = std::chrono::time_point<SystemClock, Duration>;
using SteadyPoint = std::chrono::time_point<SteadyClock, Duration>;

constexpr SystemPoint kZero = {};

inline SystemPoint SystemNow() { return SystemClock::now(); }
inline SteadyPoint SteadyNow() { return SteadyClock::now(); }

SystemPoint SystemFromSteady(SteadyPoint steady);
SteadyPoint SteadyFromSystem(SystemPoint system);

struct Timer {
  SteadyPoint steady_now = time::SteadyNow();
  SystemPoint now = time::SystemNow();
  SystemPoint last = now;
  T d = 0;  // delta from last frame
  void Tick() {
    last = now;
    now = time::SystemNow();
    steady_now = time::SteadyNow();
    d = (now - last).count();
  }
  double Now() const { return now.time_since_epoch().count(); }
};

}  // namespace automat::time