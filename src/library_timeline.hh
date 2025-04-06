// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <cmath>
#include <memory>

#include "animation.hh"
#include "base.hh"
#include "gui_button.hh"
#include "on_off.hh"
#include "run_button.hh"
#include "time.hh"
#include "timer_thread.hh"
#include "widget.hh"

namespace automat::library {

struct SideButton : gui::Button {
  using gui::Button::Button;
  SideButton(Ptr<Widget> child) : gui::Button(child) {}
  SkColor ForegroundColor() const override;
  SkColor BackgroundColor() const override;
  SkRRect RRect() const override;
};

struct PrevButton : SideButton {
  PrevButton();
  void Activate(gui::Pointer&) override;
};

struct NextButton : SideButton {
  NextButton();
  void Activate(gui::Pointer&) override;
};

struct Timeline;

struct TimelineRunButton : gui::ToggleButton {
  Timeline* timeline;

  Ptr<gui::Button> rec_button;
  mutable Ptr<gui::Button>* last_on_widget = nullptr;

  TimelineRunButton(Timeline* timeline);
  Ptr<gui::Button>& OnWidget() override;
  bool Filled() const override;
  void Activate(gui::Pointer&);
  void FixParents() override;
  void ForgetParents() override;
};

struct TrackBase : Object, Object::FallbackWidget {
  Timeline* timeline = nullptr;
  maf::Vec<time::T> timestamps;
  SkPath Shape() const override;
  void Draw(SkCanvas&) const override;
  std::unique_ptr<Action> FindAction(gui::Pointer&, gui::ActionTrigger) override;
  virtual void UpdateOutput(Location& target, time::SteadyPoint started_at,
                            time::SteadyPoint now) = 0;

  void SerializeState(Serializer& writer, const char* key) const override;
  void DeserializeState(Location& l, Deserializer& d) override;
  virtual bool TryDeserializeField(Location& l, Deserializer& d, maf::Str& field_name);
};

struct OnOffTrack : TrackBase, OnOff {
  time::T on_at = NAN;
  string_view Name() const override { return "On/Off Track"; }
  Ptr<Object> Clone() const override { return MakePtr<OnOffTrack>(*this); }
  void Draw(SkCanvas&) const override;
  void UpdateOutput(Location& target, time::SteadyPoint started_at, time::SteadyPoint now) override;

  bool IsOn() const override;
  void On() override {}
  void Off() override {}

  void SerializeState(Serializer& writer, const char* key) const override;
  void DeserializeState(Location& l, Deserializer& d) override;
  bool TryDeserializeField(Location& l, Deserializer& d, maf::Str& field_name) override;
};

// Currently Timeline pauses at end which is consistent with standard media player behavour.
// This is fine for MVP but in the future, timeline should keep playing (stuck at the end).
// The user should be able to connect the "next" connection to the "jump to start" so that it loops
// (or stops).
struct Timeline : LiveObject,
                  Object::FallbackWidget,
                  Runnable,
                  LongRunning,
                  TimerNotificationReceiver {
  Ptr<TimelineRunButton> run_button;
  Ptr<PrevButton> prev_button;
  Ptr<NextButton> next_button;

  maf::Vec<Ptr<TrackBase>> tracks;
  maf::Vec<std::unique_ptr<Argument>> track_args;

  mutable animation::Approach<> zoom;  // stores the time in seconds

  enum State { kPaused, kPlaying, kRecording } state;
  time::T timeline_length = 0;

  struct Paused {
    time::T playback_offset;  // Used when playback is paused
  };

  struct Playing {
    time::SteadyPoint started_at;  // Used when playback is active
    time::SteadyPoint now;
  };

  struct Recording {
    time::SteadyPoint started_at;  // Used when recording is active
    time::SteadyPoint now;
    // there is no point in staring the length of the timeline because it's always `now -
    // started_at`
  };

  union {
    Paused paused;
    Playing playing;
    Recording recording;
  };

  Timeline();
  Timeline(const Timeline&);
  void UpdateChildTransform();
  string_view Name() const override;
  Ptr<Object> Clone() const override;
  animation::Phase Tick(time::Timer&) override;
  void Draw(SkCanvas&) const override;
  SkPath Shape() const override;
  void Args(std::function<void(Argument&)> cb) override;
  Vec2AndDir ArgStart(const Argument&) override;
  void FillChildren(maf::Vec<Ptr<Widget>>& children) override;
  std::unique_ptr<Action> FindAction(gui::Pointer&, gui::ActionTrigger) override;
  void OnRun(Location& here) override;
  void Cancel() override;
  void OnTimerNotification(Location&, time::SteadyPoint) override;
  OnOffTrack& AddOnOffTrack(maf::StrView name);

  void BeginRecording();
  void StopRecording();

  time::T MaxTrackLength() const;

  void SerializeState(Serializer& writer, const char* key) const override;
  void DeserializeState(Location& l, Deserializer& d) override;
};

}  // namespace automat::library