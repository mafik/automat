// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "base.hh"
#include "sync.hh"
#include "time.hh"
#include "timer_thread.hh"

namespace automat::library {

struct Timer : Object, TimerNotificationReceiver {
  // Guards access to duration & LongRunning members
  std::mutex mtx;

  time::Duration duration_value = 10s;
  time::SteadyPoint start_time;
  std::unique_ptr<RunTask> long_running_task;

  SyncState runnable_sync;
  SyncState duration_sync;
  SyncState running_sync;
  NextState next_state;

  static Runnable runnable;
  static Syncable duration;
  static LongRunning timer_running;
  static NextArg next;

  enum class Range : char {
    Milliseconds,  // 0 - 1000 ms
    Seconds,       // 0 - 60 s
    Minutes,       // 0 - 60 min
    Hours,         // 0 - 12 h
    Days,          // 0 - 7 d
    EndGuard,
  } range = Range::Seconds;

  Timer();
  Timer(const Timer&);
  StrView Name() const override { return "Timer"; }
  Ptr<Object> Clone() const override;
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;
  void Interfaces(const std::function<LoopControl(Interface&)>& cb) override;
  void InterfaceName(Interface&, Str& out_name) override;
  void Updated(WeakPtr<Object>& updated) override;
  void OnTimerNotification(Location&, time::SteadyPoint) override;

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;
};

StrView ToStr(Timer::Range);
Timer::Range TimerRangeFromStr(StrView, Status&);

}  // namespace automat::library
