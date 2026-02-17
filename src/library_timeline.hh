// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <cmath>
#include <memory>

#include "base.hh"
#include "pointer.hh"
#include "run_button.hh"
#include "text_widget.hh"
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
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override = 0;

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;
};

struct OnOffTrack : TrackBase {
  time::Duration on_at = time::kDurationGuard;

  struct OnOffImpl : OnOff {
    using Parent = OnOffTrack;
    static constexpr StrView kName = "On/Off"sv;
    static constexpr int Offset() { return offsetof(OnOffTrack, on_off); }

    bool IsOn() const;
    void OnTurnOn() {}
    void OnTurnOff() {}
  };
  OnOff::Def<OnOffImpl> on_off;

  string_view Name() const override { return "On/Off Track"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(OnOffTrack, *this); }
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;
  INTERFACES(on_off)
  void Splice(time::Duration current_offset, time::Duration splice_to) override;
  void UpdateOutput(Location& target, time::SteadyPoint started_at, time::SteadyPoint now) override;

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;
};

// A track that holds a sequence of relative values.
struct Vec2Track : TrackBase {
  using ValueT = Vec2;
  Vec<Vec2> values;

  string_view Name() const override { return "Vec2 Track"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(Vec2Track, *this); }
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;
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
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;
  void Splice(time::Duration current_offset, time::Duration splice_to) override;
  void UpdateOutput(Location& target, time::SteadyPoint started_at, time::SteadyPoint now) override;

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;
};

struct TrackArgument : Argument::Table {
  Ptr<TrackBase> track;
  NestedWeakPtr<Interface::Table> end;

  TrackArgument(StrView name);
};

// Currently Timeline pauses at end which is consistent with standard media player behavour.
// This is fine for MVP but in the future, timeline should keep playing (stuck at the end).
// The user should be able to connect the "next" connection to the "jump to start" so that it loops
// (or stops).
struct Timeline : Object, TimerNotificationReceiver {
  std::mutex mutex;

  struct Run : Runnable {
    using Parent = Timeline;
    static constexpr StrView kName = "Run"sv;
    static constexpr int Offset() { return offsetof(Timeline, run); }

    void OnRun(std::unique_ptr<RunTask>&);
  };
  Runnable::Def<Run> run;

  struct Running : LongRunning {
    using Parent = Timeline;
    static constexpr StrView kName = "Running"sv;
    static constexpr int Offset() { return offsetof(Timeline, running); }

    void OnCancel();
  };
  LongRunning::Def<Running> running;

  struct NextImpl : NextArg {
    using Parent = Timeline;
    static constexpr StrView kName = "Next"sv;
    static constexpr int Offset() { return offsetof(Timeline, next); }
  };
  NextArg::Def<NextImpl> next;

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
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;
  void Interfaces(const std::function<LoopControl(Interface::Table&)>& cb) override;
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
