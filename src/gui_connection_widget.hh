// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "animation.hh"
#include "audio.hh"
#include "connector_optical.hh"
#include "widget.hh"

namespace automat {
struct Location;
struct Argument;
}  // namespace automat

namespace automat::gui {

struct ConnectionWidget;

struct DragConnectionAction : Action {
  ConnectionWidget& widget;
  std::unique_ptr<audio::Effect> effect;

  Vec2 grab_offset;
  DragConnectionAction(Pointer&, ConnectionWidget&);
  ~DragConnectionAction() override;
  void Begin() override;
  void Update() override;
  void End() override;
};

// ConnectionWidget can function in three different modes, depending on how the argument is set to
// draw:
// - Arrow: a simple arrow pointing to the target location
// - Physical Cable: a cable with a plug at the end that wiggles when moved
// - Analytically-routed Cable: a cable that always follows the nicest path
//
// TODO: separate the state of these three modes better
struct ConnectionWidget : Widget {
  Location& from;
  Argument& arg;

  struct AnimationState {
    float radar_alpha = 0;
    float radar_alpha_target = 0;
    float prototype_alpha = 0;
    float prototype_alpha_target = 0;
  };

  mutable AnimationState animation_state;

  maf::Optional<CablePhysicsSimulation>
      state;  // if the state is non-empty then the cable is physically simulated
  maf::Optional<Vec2> manual_position;  // position of the plug (bottom center)

  // Updated in `Update()`
  mutable animation::Approach<> cable_width;
  maf::Vec<Vec2AndDir> to_points;
  Location* to = nullptr;
  float transparency = 1;
  float length = 0;

  ConnectionWidget(Location&, Argument&);

  maf::StrView Name() const override { return "ConnectionWidget"; }
  SkPath Shape() const override;
  animation::Phase PreDraw(DrawContext&) const override;
  animation::Phase Update(time::Timer&) override;
  animation::Phase Draw(DrawContext&) const override;
  std::unique_ptr<Action> FindAction(Pointer&, ActionTrigger) override;
  maf::Optional<Rect> TextureBounds() const override;
};

void DrawArrow(SkCanvas& canvas, const SkPath& from_shape, const SkPath& to_shape);

}  // namespace automat::gui