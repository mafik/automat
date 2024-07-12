#pragma once

#include <stop_token>

#include "time.hh"

namespace automat {

struct Location;

// Call this only once during startup. This starts a helper thread for timers. The thread will
// run until the stop_token is set.
void StartTimeThread(std::stop_token);

struct TimerNotificationReceiver {
  virtual void OnTimerNotification(Location&, time::SteadyPoint) = 0;
};

void ScheduleAt(Location&, time::SteadyPoint);
void CancelScheduledAt(Location&);
void CancelScheduledAt(Location&, time::SteadyPoint);
void RescheduleAt(Location& here, time::SteadyPoint old_time, time::SteadyPoint new_time);

}  // namespace automat