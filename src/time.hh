// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <chrono>
#include <cmath>

namespace automat {
using std::literals::chrono_literals::operator""ms;
using std::literals::chrono_literals::operator""s;
using std::literals::chrono_literals::operator""min;
using std::literals::chrono_literals::operator""h;
}  // namespace automat

namespace automat::time {

using Duration = std::chrono::duration<int64_t, std::ratio<1, 1000000000>>;

constexpr Duration kDurationGuard = Duration(std::numeric_limits<Duration::rep>::min());
constexpr Duration kDurationInfinity = Duration(std::numeric_limits<Duration::rep>::max());

using SystemClock = std::chrono::system_clock;
using SteadyClock = std::chrono::steady_clock;
using SystemPoint = std::chrono::time_point<SystemClock, Duration>;
using SteadyPoint = std::chrono::time_point<SteadyClock, Duration>;

using FloatDuration = std::chrono::duration<double>;

inline Duration Defloat(FloatDuration d) {
  return Duration((Duration::rep)(d.count() * Duration::period::den)) / Duration::period::num;
}

constexpr double ToSeconds(FloatDuration d) { return d.count(); }
constexpr double ToSeconds(Duration d) { return ToSeconds(FloatDuration(d)); }
constexpr Duration FromSeconds(int64_t s) { return Duration(s); }
constexpr Duration FromSeconds(double s) { return Defloat(FloatDuration(s)); }

constexpr SystemPoint kZero = {};
constexpr SteadyPoint kZeroSteady = {};

inline SystemPoint SystemNow() { return SystemClock::now(); }
inline SteadyPoint SteadyNow() { return SteadyClock::now(); }

// TODO: rename because "epoch" is misleading here (it's boot time)
inline double SecondsSinceEpoch() { return ToSeconds(SteadyNow().time_since_epoch()); }

// Sawtooth wave [0, 1).
template <auto Period>
double SteadySaw() {
  uint64_t period_ticks = FromSeconds(Period).count();
  uint64_t now_ticks = SteadyNow().time_since_epoch().count();
  return (double)(now_ticks % period_ticks) / (double)(period_ticks);
}

SystemPoint SystemFromSteady(SteadyPoint steady);
SteadyPoint SteadyFromSystem(SystemPoint system);

struct Timer {
  SteadyPoint now = time::SteadyNow();
  SteadyPoint last = now;
  double d = 0;  // delta from last frame
  double NowSeconds() const { return ToSeconds(now.time_since_epoch()); }
  void Tick() {
    last = now;
    now = time::SteadyNow();
    d = ToSeconds(now - last);
  }
};

}  // namespace automat::time
