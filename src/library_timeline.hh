#pragma once

#include <memory>

#include "base.hh"
#include "run_button.hh"

namespace automat::library {

struct PrevButton : virtual gui::Button, gui::CircularButtonMixin {
  PrevButton();
  void Activate(gui::Pointer&) override;
};

struct NextButton : virtual gui::Button, gui::CircularButtonMixin {
  NextButton();
  void Activate(gui::Pointer&) override;
};

struct Timeline;

struct TrackBase : Object {
  Timeline* timeline = nullptr;
  Vec<time::T> timestamps;
  SkPath Shape() const override;
  void Draw(gui::DrawContext&) const override;
  std::unique_ptr<Action> ButtonDownAction(gui::Pointer&, gui::PointerButton) override;
};

struct OnOffTrack : TrackBase {
  string_view Name() const override { return "On/Off Track"; }
  std::unique_ptr<Object> Clone() const override { return std::make_unique<OnOffTrack>(*this); }
  void Draw(gui::DrawContext&) const override;
};

// Currently Timeline pauses at end which is consistent with standard media player behavour.
// This is fine for MVP but in the future, timeline should keep playing (stuck at the end).
// The user should be able to connect the "next" connection to the "jump to start" so that it loops
// (or stops).
struct Timeline : LiveObject, Runnable, LongRunning, TimerNotificationReceiver {
  static const Timeline proto;

  gui::RunButton run_button;
  PrevButton prev_button;
  NextButton next_button;

  Vec<std::unique_ptr<TrackBase>> tracks;

  bool currently_playing = false;
  union {
    time::T playback_offset;                // Used when playback is paused
    time::SteadyPoint playback_started_at;  // Used when playback is active
  };

  Timeline();
  Timeline(const Timeline&);
  void Relocate(Location* new_here) override;
  string_view Name() const override;
  std::unique_ptr<Object> Clone() const override;
  void Draw(gui::DrawContext&) const override;
  SkPath Shape() const override;
  void Args(std::function<void(Argument&)> cb) override;
  ControlFlow VisitChildren(gui::Visitor& visitor) override;
  SkMatrix TransformToChild(const Widget& child, animation::Context&) const override;
  std::unique_ptr<Action> ButtonDownAction(gui::Pointer&, gui::PointerButton) override;
  LongRunning* OnRun(Location& here) override;
  void Cancel() override;
  void OnTimerNotification(Location&) override;
};

}  // namespace automat::library