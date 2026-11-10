// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
#include "mux_timer.hpp"

#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "format.hpp"
#include "linux_syscall.hpp"

namespace automat::mux {

constexpr int kClockMonotonic = 1;

struct TimerSpec {
  timespec it_interval;
  timespec it_value;
};

Timer::Timer(Epoll& epoll) : epoll(epoll) {
  fd = LinuxSyscall(__NR_timerfd_create, kClockMonotonic, kNonblock);
  if (fd < 0) {
    AppendErrorMessage(status) += f("timerfd_create(): {}", strerror(-fd));
    return;
  }
  epoll.Add(this, status);
}

Timer::~Timer() {
  // The FD base member closes the timerfd; we only have to unregister it.
  if (fd >= 0) epoll.Del(this, status);
}

static char* itimerspec_dump(TimerSpec* ts) {
  static char buf[1024];
  snprintf(buf, sizeof(buf), "itimer: [ interval=%lu s %lu ns, next expire=%lu s %lu ns ]",
           ts->it_interval.tv_sec, ts->it_interval.tv_nsec, ts->it_value.tv_sec,
           ts->it_value.tv_nsec);
  return buf;
}

void Timer::Arm(double initial_s, double interval_s) {
  TimerSpec ts = {
      .it_interval = {.tv_sec = (time_t)interval_s,
                      .tv_nsec = (long)((interval_s - (uint64_t)interval_s) * 1000000000)},
      .it_value = {.tv_sec = (time_t)initial_s,
                   .tv_nsec = (long)((initial_s - (uint64_t)initial_s) * 1000000000)}};
  if (int r = LinuxSyscall(__NR_timerfd_settime, (int)fd, 0, &ts, nullptr); r < 0) {
    AppendErrorMessage(status) +=
        f("timerfd_settime(): {}: {}", itimerspec_dump(&ts), strerror(-r));
  }
}

void Timer::Disarm() { Arm(0, 0); }

void Timer::NotifyRead(Status& epoll_status) {
  uint64_t ticks;
  if (read(fd, &ticks, sizeof(ticks)) != sizeof(ticks)) {
    AppendErrorMessage(epoll_status) += "read() in Timer::NotifyRead";
    return;
  }
  if (handler) handler();
}

StrView Timer::Name() const { return "Timer"; }

}  // namespace automat::mux
