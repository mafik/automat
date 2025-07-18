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

using T = int64_t;
using Duration = std::chrono::duration<T, std::ratio<1, 1000000000>>;

constexpr Duration kDurationGuard = Duration(std::numeric_limits<T>::min());
constexpr Duration kDurationInfinity = Duration(std::numeric_limits<T>::max());

using SystemClock = std::chrono::system_clock;
using SteadyClock = std::chrono::steady_clock;
using SystemPoint = std::chrono::time_point<SystemClock, Duration>;
using SteadyPoint = std::chrono::time_point<SteadyClock, Duration>;

using FloatDuration = std::chrono::duration<double>;

inline Duration Defloat(FloatDuration d) {
  return Duration((Duration::rep)(d.count() * Duration::period::den)) / Duration::period::num;
}

inline double ToSeconds(FloatDuration d) { return d.count(); }
inline double ToSeconds(Duration d) { return ToSeconds(FloatDuration(d)); }
inline Duration FromSeconds(int64_t s) { return Duration(s); }
inline Duration FromSeconds(double s) { return Defloat(FloatDuration(s)); }

constexpr SystemPoint kZero = {};
constexpr SteadyPoint kZeroSteady = {};

inline SystemPoint SystemNow() { return SystemClock::now(); }
inline SteadyPoint SteadyNow() { return SteadyClock::now(); }

inline double SecondsSinceEpoch() { return ToSeconds(SteadyNow().time_since_epoch()); }

// Sawtooth wave [0, 1).
template <auto Period>
T SteadySaw() {
  return ToSeconds(SteadyNow().time_since_epoch() % FromSeconds(Period));
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
