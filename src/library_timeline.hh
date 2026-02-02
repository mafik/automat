// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <cmath>
#include <memory>

#include "base.hh"
#include "pointer.hh"
#include "run_button.hh"
#include "time.hh"
#include "timer_thread.hh"
#include "widget.hh"

namespace automat::library {

struct Timeline;
struct TrackArgument;

struct TrackBase : Object {
  Timeline* timeline = nullptr;
  Vec<time::Duration> timestamps;
  virtual void Splice(time::Duration current_offset, time::Duration splice_to) = 0;
  virtual void UpdateOutput(Location& target, time::SteadyPoint started_at,
                            time::SteadyPoint now) = 0;

  // Each subtype must returns its own Widget derived from TrackBaseWidget.
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent, ReferenceCounted&) override = 0;

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;
};

struct OnOffTrack : TrackBase, OnOff {
  time::Duration on_at = time::kDurationGuard;
  string_view Name() const override { return "On/Off Track"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(OnOffTrack, *this); }
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent, ReferenceCounted&) override;
  void Splice(time::Duration current_offset, time::Duration splice_to) override;
  void UpdateOutput(Location& target, time::SteadyPoint started_at, time::SteadyPoint now) override;

  bool IsOn() const override;
  void OnTurnOn() override {}
  void OnTurnOff() override {}

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;
};

// A track that holds a sequence of relative values.
struct Vec2Track : TrackBase {
  using ValueT = Vec2;
  Vec<Vec2> values;

  string_view Name() const override { return "Vec2 Track"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(Vec2Track, *this); }
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent, ReferenceCounted&) override;
  void Splice(time::Duration current_offset, time::Duration splice_to) override;
  void UpdateOutput(Location& target, time::SteadyPoint started_at, time::SteadyPoint now) override;

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;
};

// A track that holds a sequence of 64-bit floating point numbers.
struct Float64Track : TrackBase {
  using ValueT = double;
  Vec<double> values;

  string_view Name() const override { return "Float64 Track"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(Float64Track, *this); }
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent, ReferenceCounted&) override;
  void Splice(time::Duration current_offset, time::Duration splice_to) override;
  void UpdateOutput(Location& target, time::SteadyPoint started_at, time::SteadyPoint now) override;

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;
};

struct TrackArgument : InlineArgument {
  TextDrawable icon;
  Ptr<TrackBase> track;
  Str name;

  TrackArgument(StrView name);

  SkColor Tint() const override { return "#17aeb7"_color; }
  SkColor Light() const override { return "#17aeb7"_color; }
  PaintDrawable& Icon() override;
  StrView Name() const override { return name; }

  void CanConnect(Object& start, Part& end, Status& status) const override;
};

// Currently Timeline pauses at end which is consistent with standard media player behavour.
// This is fine for MVP but in the future, timeline should keep playing (stuck at the end).
// The user should be able to connect the "next" connection to the "jump to start" so that it loops
// (or stops).
struct Timeline : Object, SignalNext, TimerNotificationReceiver {
  std::mutex mutex;

  struct Run : Runnable {
    StrView Name() const override { return "Run"sv; }

    void OnRun(std::unique_ptr<RunTask>&) override;

    Timeline& GetTimeline() const {
      return *reinterpret_cast<Timeline*>(reinterpret_cast<intptr_t>(this) -
                                          offsetof(Timeline, run));
    }
  } run;

  struct Running : LongRunning {
    StrView Name() const override { return "Running"sv; }

    void OnCancel() override;

    Timeline& GetTimeline() const {
      return *reinterpret_cast<Timeline*>(reinterpret_cast<intptr_t>(this) -
                                          offsetof(Timeline, running));
    }
  } running;

  Vec<std::unique_ptr<TrackArgument>> tracks;

  float zoom;  // stores the time in seconds

  enum State { kPaused, kPlaying, kRecording } state;
  time::Duration timeline_length;

  struct Paused {
    time::Duration playback_offset;  // Used when playback is paused
  };

  struct Playing {
    time::SteadyPoint started_at;  // Used when playback is active
  };

  struct Recording {
    time::SteadyPoint started_at;  // Used when recording is active
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
  string_view Name() const override;
  Ptr<Object> Clone() const override;
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent, ReferenceCounted&) override;
  void Parts(const std::function<void(Part&)>& cb) override;
  LongRunning* AsLongRunning() override { return &running; }
  void OnTimerNotification(Location&, time::SteadyPoint) override;
  OnOffTrack& AddOnOffTrack(StrView name);
  Vec2Track& AddVec2Track(StrView name);
  Float64Track& AddFloat64Track(StrView name);

  void AddTrack(Ptr<TrackBase>&& track, StrView name);

  void BeginRecording();
  void StopRecording();

  time::Duration CurrentOffset(time::SteadyPoint now) const;
  time::Duration MaxTrackLength() const;

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;
};

}  // namespace automat::library
