#pragma once

#include <chrono>

#include "base.hh"

namespace automat::library {

using Clock = std::chrono::steady_clock;
using Duration = std::chrono::duration<double>;
using TimePoint = std::chrono::time_point<Clock, Duration>;

struct TimerDelay : LiveObject {
  Duration duration = 10s;
  TimePoint start_time;
  enum class State : char { IDLE, RUNNING } state = State::IDLE;
  enum class OverrunPolicy : char {
    IGNORE,
    TOGGLE,
    RESTART,
    EXTEND
  } overrun_policy = OverrunPolicy::TOGGLE;
  // TODO: range switching
  enum class Range : char {
    Milliseconds,  // 0 - 1000 ms
    Seconds,       // 0 - 60 s
    Minutes,       // 0 - 60 min
    Hours,         // 0 - 12 h
    Days,          // 0 - 7 d
  } range = Range::Seconds;
  static const TimerDelay proto;
  static Argument finished_arg;
  TimerDelay();
  string_view Name() const override;
  std::unique_ptr<Object> Clone() const override;
  void Draw(gui::DrawContext&) const override;
  SkPath Shape() const override;
  std::unique_ptr<Action> ButtonDownAction(gui::Pointer&, gui::PointerButton) override;
  void Args(std::function<void(Argument&)> cb) override;
  void Relocate(Location* here) override {
    LOG << "Relocating TimerDelay";
    this->here = here;
  }
  void Run(Location& here) override;
};

}  // namespace automat::library