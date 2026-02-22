// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "base.hh"
#include "color.hh"
#include "sync.hh"
#include "time.hh"
#include "timer_thread.hh"

namespace automat::library {

struct Timer : Object, TimerNotificationReceiver {
  // Guards access to duration & LongRunning members
  std::mutex mtx;

  time::Duration duration_value = 10s;
  time::SteadyPoint start_time;

  DEF_INTERFACE(Timer, Syncable, duration, "Duration")
    static constexpr SkColor kTint = "#6e4521"_color;
    bool CanSync(Syncable other) {
      return other.table == &Syncable::Def<duration_Impl>::tbl;
    }
  DEF_END(duration);

  DEF_INTERFACE(Timer, Runnable, run, "Run")
    void OnRun(std::unique_ptr<RunTask>& run_task) { obj->StartTimer(run_task); }
  DEF_END(run);

  DEF_INTERFACE(Timer, LongRunning, running, "Running")
    void OnCancel() { obj->CancelTimer(); }
  DEF_END(running);

  DEF_INTERFACE(Timer, NextArg, next, "Next")
  DEF_END(next);

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
  void StartTimer(std::unique_ptr<RunTask>&);
  void CancelTimer();
  StrView Name() const override { return "Timer"; }
  Ptr<Object> Clone() const override;
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;
  INTERFACES(run, duration, next, running)
  void Updated(WeakPtr<Object>& updated) override;
  void OnTimerNotification(Location&, time::SteadyPoint) override;

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;
};

StrView ToStr(Timer::Range);
Timer::Range TimerRangeFromStr(StrView, Status&);

}  // namespace automat::library
