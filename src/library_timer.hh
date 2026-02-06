// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "base.hh"
#include "sync.hh"
#include "time.hh"
#include "timer_thread.hh"

namespace automat::library {

struct Timer : Object, Runnable, TimerNotificationReceiver {
  // Guards access to duration & LongRunning members
  std::mutex mtx;
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
    Timer& GetTimer() const {
      return *reinterpret_cast<Timer*>(reinterpret_cast<intptr_t>(this) -
                                       offsetof(Timer, timer_running));
    }
    Object* OnFindRunnable() override { return &GetTimer(); }
    void OnCancel() override;
  } timer_running;

  Timer();
  Timer(const Timer&);
  StrView Name() const override { return "Timer"; }
  Ptr<Object> Clone() const override;
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;
  void Parts(const std::function<void(Part&)>& cb) override;
  void PartName(Part& part, Str& out_name) override;
  void OnRun(std::unique_ptr<RunTask>&) override;
  LongRunning* AsLongRunning() override { return &timer_running; }
  void Updated(WeakPtr<Object>& updated) override;
  void OnTimerNotification(Location&, time::SteadyPoint) override;

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;
};

StrView ToStr(Timer::Range);
Timer::Range TimerRangeFromStr(StrView, Status&);

}  // namespace automat::library
