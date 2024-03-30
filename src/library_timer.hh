#pragma once

#include <chrono>

#include "animation.hh"
#include "base.hh"
#include "number_text_field.hh"

namespace automat::library {

using Clock = std::chrono::steady_clock;
using Duration = std::chrono::duration<double>;
using TimePoint = std::chrono::time_point<Clock, Duration>;

struct TimerDelay : LiveObject {
  Duration duration = 10s;
  TimePoint start_time;
  mutable animation::Approach start_pusher_depression;
  mutable animation::Approach left_pusher_depression;
  mutable animation::Approach right_pusher_depression;
  mutable animation::Spring hand_degrees;
  mutable animation::Spring range_dial;
  mutable animation::Approach duration_handle_rotation;
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
  static Argument finished_arg;
  TimerDelay();
  string_view Name() const override;
  std::unique_ptr<Object> Clone() const override;
  void Draw(gui::DrawContext&) const override;
  SkPath Shape() const override;
  SkPath ArgShape(Argument&) const override;
  std::unique_ptr<Action> ButtonDownAction(gui::Pointer&, gui::PointerButton) override;
  void Args(std::function<void(Argument&)> cb) override;
  void Run(Location& here) override;
  void Updated(Location& here, Location& updated) override;
  ControlFlow VisitChildren(gui::Visitor& visitor) override;
  SkMatrix TransformToChild(const Widget& child, animation::Context&) const override;
};

// Call this only once during startup. This starts a helper thread for timers. The thread will
// run until the stop_token is set.
void StartTimerHelperThread(std::stop_token);

}  // namespace automat::library