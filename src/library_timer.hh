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

  struct DurationImpl : Syncable {
    using Parent = Timer;
    static constexpr StrView kName = "Duration"sv;
    static constexpr SkColor kTint = "#6e4521"_color;
    static constexpr int Offset() { return offsetof(Timer, duration); }

    bool CanSync(Syncable other) {
      return other.table == &Syncable::Def<DurationImpl>::GetTable();
    }
  };
  Syncable::Def<DurationImpl> duration;

  struct Run : Runnable {
    using Parent = Timer;
    static constexpr StrView kName = "Run"sv;
    static constexpr int Offset() { return offsetof(Timer, run); }

    void OnRun(std::unique_ptr<RunTask>&);
  };
  Runnable::Def<Run> run;

  struct Running : LongRunning {
    using Parent = Timer;
    static constexpr StrView kName = "Running"sv;
    static constexpr int Offset() { return offsetof(Timer, running); }

    void OnCancel();
  };
  LongRunning::Def<Running> running;

  struct Next : NextArg {
    using Parent = Timer;
    static constexpr StrView kName = "Next"sv;
    static constexpr int Offset() { return offsetof(Timer, next); }
  };
  NextArg::Def<Next> next;

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
  INTERFACES(run, duration, next, running)
  void Updated(WeakPtr<Object>& updated) override;
  void OnTimerNotification(Location&, time::SteadyPoint) override;

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;
};

StrView ToStr(Timer::Range);
Timer::Range TimerRangeFromStr(StrView, Status&);

}  // namespace automat::library
