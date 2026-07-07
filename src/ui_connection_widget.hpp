#pragma once
// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT

#include "animation.hpp"
#include "argument.hpp"
#include "audio.hpp"
#include "connector_optical.hpp"
#include "object.hpp"
#include "root_widget.hpp"
#include "stream.hpp"
#include "widget.hpp"

namespace automat {
struct Location;
struct Argument;
}  // namespace automat

namespace automat::ui {

struct ConnectionWidget;

struct DragConnectionAction : Action {
  MortalPtr<ConnectionWidget> widget;
  std::unique_ptr<audio::Effect> effect;

  Vec2 grab_offset;
  DragConnectionAction(Pointer&, ConnectionWidget&);
  ~DragConnectionAction() override;
  void Update() override;
  bool Highlight(Interface) const override;
};

// ConnectionWidget can function in three different modes, depending on how the argument is set to
// draw:
// - Arrow: a simple arrow pointing to the target location
// - Physical Cable: a cable with a plug at the end that wiggles when moved
// - Analytically-routed Cable: a cable that always follows the nicest path
//
// TODO: separate the state of these three modes better
struct ConnectionWidget : ArgumentToy {
  struct AnimationState {
    float radar_alpha = 0;
    float radar_alpha_target = 0;
    float prototype_alpha = 0;
    float prototype_alpha_target = 0;
    double time_seconds = 0;
  };

  mutable AnimationState animation_state;

  Optional<CablePhysicsSimulation>
      state;  // if the state is non-empty then the cable is physically simulated
  Optional<Vec2> manual_position;  // position of the plug (bottom center)

  // Updated in `Tick()`
  Argument::Table::Style style;
  SkColor tint;
  Vec2AndDir pos_dir;  // position of connection start
  SkPath from_shape;   // board coords
  SkPath to_shape;     // board coords
  animation::Approach<> cable_width;
  Vec<Vec2AndDir> to_points;
  float transparency = 1;
  float alpha = 0;
  float length = 0;
  Optional<ArcLine> arcline;        // routed cable for non-physical connections
  Optional<Vec2> end_anchor_local;  // cable end in the end widget's local frame

  // Stream style only: the dash pattern inside the bore advances with the
  // measured byte rate; the format and rate are printed beside the midpoint.
  RateEstimator stream_byte_rate;
  RateEstimator stream_unit_rate;
  float stream_dash_phase = 0;
  float stream_bytes_per_s = 0;
  float stream_units_per_s = 0;
  float stream_rate_drawn = 0;
  uint64_t stream_fill = 0;
  uint64_t stream_capacity = 0;
  StrView stream_fill_unit = {};
  float stream_fill_drawn = 0;
  // The blocked side flickers under flow (a fast producer is momentarily
  // blocked on many samples and free on others), so each side accumulates a
  // score and is shown only while its score stays high.
  StreamBlocked stream_blocked = StreamBlocked::None;
  float stream_blocked_score = 0;
  bool stream_blocked_shown = false;
  Str stream_format;
  // A refused manual link: the oracle's reason (the two irreconcilable
  // formats, proposed adapters), printed below the port until the deadline.
  Str refusal_text;
  time::SteadyPoint refusal_until = {};
  std::unique_ptr<ui::Widget> icon;
  std::unique_ptr<ui::Widget> spotlight;
  std::unique_ptr<ui::Widget> radar;
  std::unique_ptr<ui::Widget> prototype_ghost;

  ConnectionWidget(Widget* parent, Object&, Argument::Table&);

  Location* StartLocation() const;  // TODO: remove
  Location* EndLocation() const;    // TODO: remove

  StrView Name() const override { return "ConnectionWidget"; }
  void ShowRefusal(Str text);
  SkPath Shape() const override;
  Tock Tick(time::Timer&) override;
  void Draw(SkCanvas&) const override;
  Compositor GetCompositor() const override { return Compositor::ANCHOR_WARP; }
  std::unique_ptr<Action> FindAction(Pointer&, ActionTrigger) override;
  Optional<Rect> TextureBounds() const override;
  Vec<Vec2> TextureAnchors() override;
};

// Now that ConnectionWidget is defined, we can check whether Argument can make toys
static_assert(ToyMaker<Argument>);

void DrawArrow(SkCanvas& canvas, const SkPath& from_shape, const SkPath& to_shape);

}  // namespace automat::ui
