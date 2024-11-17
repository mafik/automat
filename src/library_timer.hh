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

struct TimerDelay : LiveObject, Runnable, LongRunning, TimerNotificationReceiver {
  struct MyDuration : Object {
    time::Duration value = 10s;
    std::shared_ptr<Object> Clone() const override { return std::make_shared<MyDuration>(*this); }
  } duration;
  DurationArgument duration_arg;
  time::SteadyPoint start_time;
  mutable animation::Approach<> start_pusher_depression;
  mutable animation::Approach<> left_pusher_depression;
  mutable animation::Approach<> right_pusher_depression;
  mutable animation::SpringV2<float> hand_degrees;
  int hand_draggers = 0;
  // Controls the current range (milliseconds, seconds, etc.)
  mutable animation::SpringV2<float> range_dial;
  mutable animation::Approach<> duration_handle_rotation;
  std::shared_ptr<gui::NumberTextField> text_field;
  enum class Range : char {
    Milliseconds,  // 0 - 1000 ms
    Seconds,       // 0 - 60 s
    Minutes,       // 0 - 60 min
    Hours,         // 0 - 12 h
    Days,          // 0 - 7 d
    EndGuard,
  } range = Range::Seconds;
  static std::shared_ptr<TimerDelay> proto;
  TimerDelay();
  TimerDelay(const TimerDelay&);
  string_view Name() const override;
  std::shared_ptr<Object> Clone() const override;
  animation::Phase Draw(gui::DrawContext&) const override;
  SkPath Shape() const override;
  void Fields(std::function<void(Object&)> cb) override;
  SkPath FieldShape(Object&) const override;
  std::unique_ptr<Action> FindAction(gui::Pointer&, gui::ActionTrigger) override;
  void Args(std::function<void(Argument&)> cb) override;
  LongRunning* OnRun(Location& here) override;
  void Cancel() override;
  void Updated(Location& here, Location& updated) override;
  ControlFlow VisitChildren(gui::Visitor& visitor) override;
  SkMatrix TransformToChild(const Widget& child, animation::Display*) const override;
  void OnTimerNotification(Location&, time::SteadyPoint) override;

  void SerializeState(Serializer& writer, const char* key) const override;
  void DeserializeState(Location& l, Deserializer& d) override;
};

maf::StrView ToStr(TimerDelay::Range);
TimerDelay::Range TimerRangeFromStr(maf::StrView, maf::Status&);

}  // namespace automat::library