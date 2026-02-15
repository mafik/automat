// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "base.hh"
#include "parent_ref.hh"
#include "sync.hh"
#include "time.hh"
#include "timer_thread.hh"

namespace automat::library {

struct Timer : Object, TimerNotificationReceiver {
  // Guards access to duration & LongRunning members
  std::mutex mtx;
  struct MyRunnable : Runnable {
    StrView Name() const override { return "Run"sv; }
    void OnRun(std::unique_ptr<RunTask>&) override;
    PARENT_REF(Timer, runnable)
  } runnable;
  struct MyDuration : Syncable {
    time::Duration value = 10s;
    StrView Name() const override { return "duration"sv; }
    SkColor Tint() const override { return "#6e4521"_color; }
    bool CanSync(const Syncable& other) const override {
      return dynamic_cast<const MyDuration*>(&other) != nullptr;
    }
  } duration;
  time::SteadyPoint start_time;
  enum class Range : char {
    Milliseconds,  // 0 - 1000 ms
    Seconds,       // 0 - 60 s
    Minutes,       // 0 - 60 min
    Hours,         // 0 - 12 h
    Days,          // 0 - 7 d
    EndGuard,
  } range = Range::Seconds;
  struct TimerRunning : LongRunning {
    PARENT_REF(Timer, timer_running)
    Object* OnFindRunnable() override { return &Timer(); }
    void OnCancel() override;
  } timer_running;

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
