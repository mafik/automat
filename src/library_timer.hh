#pragma once

#include "animation.hh"
#include "base.hh"
#include "number_text_field.hh"
#include "time.hh"

namespace automat::library {

struct DurationArgument : LiveArgument {
  DurationArgument();
};

struct TimerDelay : LiveObject, Runnable, LongRunning, TimerNotificationReceiver {
  struct MyDuration : Object {
    time::Duration value = 10s;
    std::unique_ptr<Object> Clone() const override { return std::make_unique<MyDuration>(*this); }
  } duration;
  DurationArgument duration_arg;
  time::SteadyPoint start_time;
  mutable animation::Approach<> start_pusher_depression;
  mutable animation::Approach<> left_pusher_depression;
  mutable animation::Approach<> right_pusher_depression;
  mutable animation::Spring<float> hand_degrees;
  mutable animation::Spring<float> range_dial;
  mutable animation::Approach<> duration_handle_rotation;
  gui::NumberTextField text_field;
  enum class State : char { Idle, Running } state = State::Idle;
  enum class OverrunPolicy : char {
    Ignore,
    Toggle,
    Restart,
    Extend
  } overrun_policy = OverrunPolicy::Toggle;
  enum class Range : char {
    Milliseconds,  // 0 - 1000 ms
    Seconds,       // 0 - 60 s
    Minutes,       // 0 - 60 min
    Hours,         // 0 - 12 h
    Days,          // 0 - 7 d
    EndGuard,
  } range = Range::Seconds;
  static const TimerDelay proto;
  TimerDelay();
  TimerDelay(const TimerDelay&);
  string_view Name() const override;
  std::unique_ptr<Object> Clone() const override;
  void Draw(gui::DrawContext&) const override;
  SkPath Shape() const override;
  void Fields(std::function<void(Object&)> cb) override;
  SkPath FieldShape(Object&) const override;
  std::unique_ptr<Action> ButtonDownAction(gui::Pointer&, gui::PointerButton) override;
  void Args(std::function<void(Argument&)> cb) override;
  LongRunning* OnRun(Location& here) override;
  void Cancel() override;
  void Updated(Location& here, Location& updated) override;
  ControlFlow VisitChildren(gui::Visitor& visitor) override;
  SkMatrix TransformToChild(const Widget& child, animation::Display*) const override;
  void OnTimerNotification(Location&, time::SteadyPoint) override;
};

}  // namespace automat::library