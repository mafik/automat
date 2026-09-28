#pragma once
// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT

#include "base.hpp"
#include "color.hpp"
#include "command.hpp"
#include "sync.hpp"
#include "time.hpp"
#include "timer_thread.hpp"

namespace automat::library {

struct Timer : Object, TimerNotificationReceiver {
  // Guards access to duration & LongRunning members
  std::mutex mtx;

  time::Duration duration_value = 10s;
  time::SteadyPoint start_time;

  // Disabled until there are more objects that could be
  // combined with duration (for example some math objects)
  // DEF_INTERFACE(Timer, Argument, duration, "Duration")
  // static constexpr SkColor kTint = "#6e4521"_color;
  // bool CanSync(Syncable other) { return other.table == &Syncable::Def<duration_Impl>::tbl; }
  // DEF_END(duration);

  DEF_INTERFACE(Timer, Command, running, "Running")
  static constexpr ui::Cursor kCursor = ui::Cursor::Hand;
  void OnRun() { obj->StartTimer(); }
  void OnCancel() { obj->CancelTimer(); }
  DEF_END(running);

  DEF_INTERFACE(Timer, NextArg, next, "Next")
  DEF_END(next);

  DEF_INTERFACE(Timer, Scalar, duration, "Duration")
  double OnGet();
  void OnSet(double seconds);
  std::unique_ptr<Action> OnActivate(ui::Pointer&, automat::Toy*);
  DEF_END(duration);

  DEF_INTERFACE(Timer, Text, readout, "Readout")
  Str OnGet();
  void OnSet(StrView);
  DEF_END(readout);

  DEF_INTERFACE(Timer, BasicCommand, next_range, "Next range")
  static constexpr ui::Cursor kCursor = ui::Cursor::Hand;
  void OnRun() { obj->ShiftRange(1); }
  DEF_END(next_range);

  DEF_INTERFACE(Timer, BasicCommand, prev_range, "Previous range")
  static constexpr ui::Cursor kCursor = ui::Cursor::Hand;
  void OnRun() { obj->ShiftRange(-1); }
  DEF_END(prev_range);

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
  void StartTimer();
  void CancelTimer();
  void ShiftRange(int delta);
  StrView Name() const override { return "Timer"; }
  Ptr<Object> Clone() const override;
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;
  INTERFACES(running_tbl, next_tbl, duration_tbl, next_range_tbl, prev_range_tbl, readout_tbl);
  void Updated(WeakPtr<Object>& updated) override;
  void OnTimerNotification(Location&, time::SteadyPoint) override;

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;
};

StrView ToStr(Timer::Range);
Timer::Range TimerRangeFromStr(StrView, Status&);

}  // namespace automat::library
