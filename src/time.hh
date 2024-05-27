#pragma once

#include <chrono>
#include <stop_token>

namespace automat {

namespace time {

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
  SystemPoint now = time::SystemNow();
  SystemPoint last = now;
  T d = 0;  // delta from last frame
  void Tick() {
    last = now;
    now = time::SystemNow();
    d = (now - last).count();
  }
  double Now() const { return now.time_since_epoch().count(); }
};

}  // namespace time

struct Location;

// Call this only once during startup. This starts a helper thread for timers. The thread will
// run until the stop_token is set.
void StartTimeThread(std::stop_token);

struct TimerNotificationReceiver {
  virtual void OnTimerNotification(Location&) = 0;
};

void ScheduleAt(Location&, time::SteadyPoint);
void CancelScheduledAt(Location&, time::SteadyPoint);
void RescheduleAt(Location& here, time::SteadyPoint old_time, time::SteadyPoint new_time);

}  // namespace automat