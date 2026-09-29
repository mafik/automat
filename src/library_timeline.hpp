#pragma once
// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT

#include <cmath>
#include <memory>

#include "base.hpp"
#include "command.hpp"
#include "pointer.hpp"
#include "time.hpp"
#include "timer_thread.hpp"
#include "widget.hpp"

namespace automat::library {

struct Timeline;

struct TrackBase : Object {
  Str name_str;

  TrackBase(Str name, std::initializer_list<Interface::Table*> tables);
  TrackBase(const TrackBase&);

  InterfaceArgument<Interface>::State arg_state;
  InterfaceArgument<Interface>::Table arg_table;

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

  DEF_INTERFACE(OnOffTrack, OnOff, on_off, "On/Off")
  bool IsOn() const;
  void OnTurnOn() {}
  void OnTurnOff() {}
  DEF_END(on_off);

  OnOffTrack(Str name) : TrackBase(std::move(name), {&on_off_tbl}) {}
  string_view Name() const override { return "On/Off Track"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(OnOffTrack, *this); }
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;
  void Splice(time::Duration current_offset, time::Duration splice_to) override;
  void UpdateOutput(Location& target, time::SteadyPoint started_at, time::SteadyPoint now) override;

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;
};

// A track that holds a sequence of relative values.
struct Vec2Track : TrackBase {
  using ValueT = Vec2;
  Vec<Vec2> values;

  Vec2Track(Str name) : TrackBase(std::move(name), {}) {}
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

  Float64Track(Str name) : TrackBase(std::move(name), {}) {}
  string_view Name() const override { return "Float64 Track"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(Float64Track, *this); }
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;
  void Splice(time::Duration current_offset, time::Duration splice_to) override;
  void UpdateOutput(Location& target, time::SteadyPoint started_at, time::SteadyPoint now) override;

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;
};

// Currently Timeline pauses at end which is consistent with standard media player behavour.
// This is fine for MVP but in the future, timeline should keep playing (stuck at the end).
// The user should be able to connect the "next" connection to the "jump to start" so that it loops
// (or stops).
struct Timeline : Object, TimerNotificationReceiver {
  std::mutex mutex;

  DEF_INTERFACE(Timeline, Command, running, "Running")
  void OnRun() { obj->Play(); }
  void OnCancel() { obj->Pause(); }
  DEF_END(running);

  DEF_INTERFACE(Timeline, NextArg, next, "Next")
  DEF_END(next);

  DEF_INTERFACE(Timeline, Scalar, position, "Position")
  double OnGet() {
    auto lock = std::lock_guard(obj->mutex);
    return time::ToSeconds(obj->CurrentOffset(time::SteadyNow()));
  }
  void OnSet(double seconds) {
    auto lock = std::lock_guard(obj->mutex);
    obj->SetOffset(time::FromSeconds(seconds), time::SteadyNow());
    obj->WakeToys();
  }
  std::unique_ptr<Action> OnActivate(ui::Pointer&, automat::Toy*);
  DEF_END(position);

  DEF_INTERFACE(Timeline, Scalar, zoom, "Zoom")
  double OnGet() {
    auto lock = std::lock_guard(obj->mutex);
    return obj->zoom_seconds;
  }
  void OnSet(double seconds) {
    auto lock = std::lock_guard(obj->mutex);
    obj->zoom_seconds = std::clamp<float>(seconds, 0.001f, 3600.0f);
    obj->WakeToys();
  }
  std::unique_ptr<Action> OnActivate(ui::Pointer&, automat::Toy*);
  DEF_END(zoom);

  DEF_INTERFACE(Timeline, BasicCommand, jump_to_start, "Jump to start")
  void OnRun();
  DEF_END(jump_to_start);

  DEF_INTERFACE(Timeline, BasicCommand, jump_to_end, "Jump to end")
  void OnRun();
  DEF_END(jump_to_end);

  DEF_INTERFACE(Timeline, BasicCommand, stop_recording, "Stop recording")
  void OnRun();
  DEF_END(stop_recording);

  INTERFACES(next_tbl, running_tbl, position_tbl, zoom_tbl, jump_to_start_tbl, jump_to_end_tbl,
             stop_recording_tbl);

  Vec<Owned<TrackBase>> tracks;

  float zoom_seconds;

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
  void Play();
  void Pause();
  Ptr<Object> Clone() const override;
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;
  LoopControl Interfaces(const std::function<LoopControl(Interface)>& cb) override;
  void OnTimerNotification(Location&, time::SteadyPoint) override;
  OnOffTrack& AddOnOffTrack(Str name);
  Vec2Track& AddVec2Track(Str name);
  Float64Track& AddFloat64Track(Str name);

  void AddTrack(Ptr<TrackBase>&& track);

  void BeginRecording();
  void StopRecording();

  time::Duration CurrentOffset(time::SteadyPoint now) const;
  void SetOffset(time::Duration offset, time::SteadyPoint now);
  time::Duration MaxTrackLength() const;

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;
};

}  // namespace automat::library
