// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "animation.hh"
#include "base.hh"
#include "number_text_field.hh"
#include "time.hh"
#include "timer_thread.hh"

namespace automat::library {

struct DurationArgument : LiveArgument {
  DurationArgument();
};

struct TimerDelay : LiveObject,
                    Object::FallbackWidget,
                    Runnable,
                    LongRunning,
                    TimerNotificationReceiver {
  struct MyDuration : public Object {
    time::Duration value = 10s;
    Ptr<Object> Clone() const override { return MAKE_PTR(MyDuration, *this); }
  } duration;
  DurationArgument duration_arg;
  time::SteadyPoint start_time;
  float start_pusher_depression = 0;
  float left_pusher_depression = 0;
  float right_pusher_depression = 0;
  animation::SpringV2<float> hand_degrees;
  int hand_draggers = 0;
  // Controls the current range (milliseconds, seconds, etc.)
  animation::SpringV2<float> range_dial;
  float duration_handle_rotation = 0;
  std::unique_ptr<ui::NumberTextField> text_field;
  enum class Range : char {
    Milliseconds,  // 0 - 1000 ms
    Seconds,       // 0 - 60 s
    Minutes,       // 0 - 60 min
    Hours,         // 0 - 12 h
    Days,          // 0 - 7 d
    EndGuard,
  } range = Range::Seconds;
  TimerDelay(ui::Widget* parent);
  TimerDelay(const TimerDelay&);
  string_view Name() const override;
  Ptr<Object> Clone() const override;
  animation::Phase Tick(time::Timer&) override;
  void Draw(SkCanvas&) const override;
  SkPath Shape() const override;
  void Fields(std::function<void(Object&)> cb) override;
  SkPath FieldShape(Object&) const override;
  std::unique_ptr<Action> FindAction(ui::Pointer&, ui::ActionTrigger) override;
  void Args(std::function<void(Argument&)> cb) override;
  void OnRun(Location& here, RunTask&) override;
  void OnCancel() override;
  LongRunning* AsLongRunning() override { return this; }
  void Updated(Location& here, Location& updated) override;
  void FillChildren(Vec<Widget*>& children) override;
  void OnTimerNotification(Location&, time::SteadyPoint) override;

  void SerializeState(Serializer& writer, const char* key) const override;
  void DeserializeState(Location& l, Deserializer& d) override;
};

StrView ToStr(TimerDelay::Range);
TimerDelay::Range TimerRangeFromStr(StrView, Status&);

}  // namespace automat::library