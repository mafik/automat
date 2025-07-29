// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <cmath>
#include <memory>

#include "animation.hh"
#include "base.hh"
#include "ui_button.hh"
#include "on_off.hh"
#include "pointer.hh"
#include "run_button.hh"
#include "time.hh"
#include "timer_thread.hh"
#include "widget.hh"

namespace automat::library {

struct SideButton : ui::Button {
  using ui::Button::Button;
  SideButton(Widget* parent, std::unique_ptr<Widget> child)
      : ui::Button(parent, std::move(child)) {}
  SkColor ForegroundColor() const override;
  SkColor BackgroundColor() const override;
  SkRRect RRect() const override;
};

struct PrevButton : SideButton {
  PrevButton(Widget* parent);
  void Activate(ui::Pointer&) override;
};

struct NextButton : SideButton {
  NextButton(Widget* parent);
  void Activate(ui::Pointer&) override;
};

struct Timeline;

struct TimelineRunButton : ui::ToggleButton {
  Timeline* timeline;

  std::unique_ptr<ui::Button> rec_button;
  mutable ui::Button* last_on_widget = nullptr;

  TimelineRunButton(Widget* parent, Timeline* timeline);
  ui::Button* OnWidget() override;
  bool Filled() const override;
  void Activate(ui::Pointer&);
};

struct TrackBase : Object {
  Timeline* timeline = nullptr;
  Vec<time::Duration> timestamps;
  virtual void Splice(time::Duration current_offset, time::Duration splice_to) = 0;
  virtual void UpdateOutput(Location& target, time::SteadyPoint started_at,
                            time::SteadyPoint now) = 0;

  // Each subtype must returns its own Widget derived from TrackBaseWidget.
  std::unique_ptr<ui::Widget> MakeWidget(ui::Widget* parent) override = 0;

  void SerializeState(Serializer& writer, const char* key) const override;
  void DeserializeState(Location& l, Deserializer& d) override;
  virtual bool TryDeserializeField(Location& l, Deserializer& d, Str& field_name);
};

struct OnOffTrack : TrackBase, OnOff {
  time::Duration on_at = time::kDurationGuard;
  string_view Name() const override { return "On/Off Track"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(OnOffTrack, *this); }
  std::unique_ptr<ui::Widget> MakeWidget(ui::Widget* parent) override;
  void Splice(time::Duration current_offset, time::Duration splice_to) override;
  void UpdateOutput(Location& target, time::SteadyPoint started_at, time::SteadyPoint now) override;

  bool IsOn() const override;
  void On() override {}
  void Off() override {}

  void SerializeState(Serializer& writer, const char* key) const override;
  void DeserializeState(Location& l, Deserializer& d) override;
  bool TryDeserializeField(Location& l, Deserializer& d, Str& field_name) override;
};

// A track that holds a sequence of relative values.
struct Vec2Track : TrackBase {
  Vec<Vec2> values;

  string_view Name() const override { return "Vec2 Track"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(Vec2Track, *this); }
  std::unique_ptr<ui::Widget> MakeWidget(ui::Widget* parent) override;
  void Splice(time::Duration current_offset, time::Duration splice_to) override;
  void UpdateOutput(Location& target, time::SteadyPoint started_at, time::SteadyPoint now) override;

  void SerializeState(Serializer& writer, const char* key) const override;
  void DeserializeState(Location& l, Deserializer& d) override;
  bool TryDeserializeField(Location& l, Deserializer& d, Str& field_name) override;
};

struct SpliceAction : Action {
  Timeline& timeline;
  time::Duration splice_to;
  bool snapped = false;
  bool cancel = true;
  ui::Pointer::IconOverride resize_icon;
  SpliceAction(ui::Pointer& pointer, Timeline& timeline);
  ~SpliceAction();
  void Update() override;
};

struct DragZoomAction;

// Currently Timeline pauses at end which is consistent with standard media player behavour.
// This is fine for MVP but in the future, timeline should keep playing (stuck at the end).
// The user should be able to connect the "next" connection to the "jump to start" so that it loops
// (or stops).
struct Timeline : LiveObject,
                  Object::FallbackWidget,
                  Runnable,
                  LongRunning,
                  TimerNotificationReceiver {
  std::unique_ptr<TimelineRunButton> run_button;
  std::unique_ptr<PrevButton> prev_button;
  std::unique_ptr<NextButton> next_button;

  SpliceAction* splice_action = nullptr;
  DragZoomAction* drag_zoom_action = nullptr;

  Vec<Ptr<TrackBase>> tracks;
  Vec<std::unique_ptr<Argument>> track_args;
  mutable Vec<std::unique_ptr<Widget>> track_widgets;

  mutable animation::Approach<> zoom;  // stores the time in seconds
  mutable animation::SpringV2<float> splice_wiggle;
  mutable float bridge_wiggle_s;
  bool bridge_snapped = false;

  enum State { kPaused, kPlaying, kRecording } state;
  time::Duration timeline_length = 0s;

  struct Paused {
    time::Duration playback_offset;  // Used when playback is paused
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

  Timeline(ui::Widget* parent);
  Timeline(ui::Widget* parent, const Timeline&);
  void UpdateChildTransform(time::SteadyPoint now);
  string_view Name() const override;
  Ptr<Object> Clone() const override;
  animation::Phase Tick(time::Timer&) override;
  void Draw(SkCanvas&) const override;
  SkPath Shape() const override;
  void Args(std::function<void(Argument&)> cb) override;
  Vec2AndDir ArgStart(const Argument&) override;
  void FillChildren(Vec<Widget*>& children) override;
  std::unique_ptr<Action> FindAction(ui::Pointer&, ui::ActionTrigger) override;
  void OnRun(Location& here, RunTask&) override;
  void OnCancel() override;
  LongRunning* AsLongRunning() override { return this; }
  void OnTimerNotification(Location&, time::SteadyPoint) override;
  OnOffTrack& AddOnOffTrack(StrView name);
  Vec2Track& AddVec2Track(StrView name);

  void AddTrack(Ptr<TrackBase>&& track, StrView name);

  void BeginRecording();
  void StopRecording();

  time::Duration MaxTrackLength() const;

  void SerializeState(Serializer& writer, const char* key) const override;
  void DeserializeState(Location& l, Deserializer& d) override;
};

}  // namespace automat::library