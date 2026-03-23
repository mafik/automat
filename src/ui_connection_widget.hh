// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "animation.hh"
#include "argument.hh"
#include "audio.hh"
#include "connector_optical.hh"
#include "object.hh"
#include "root_widget.hh"
#include "widget.hh"

namespace automat {
struct Location;
struct Argument;
}  // namespace automat

namespace automat::ui {

struct ConnectionWidget;

struct DragConnectionAction : Action {
  ConnectionWidget& widget;
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
  mutable animation::Approach<> cable_width;
  Vec<Vec2AndDir> to_points;
  float transparency = 1;
  float alpha = 0;
  float length = 0;
  mutable std::unique_ptr<ObjectToy> prototype_widget;
  std::unique_ptr<ui::Widget> icon;

  ConnectionWidget(Widget* parent, Object&, Argument::Table&);

  Location* StartLocation() const;  // TODO: remove
  Location* EndLocation() const;    // TODO: remove

  StrView Name() const override { return "ConnectionWidget"; }
  SkPath Shape() const override;
  void PreDraw(SkCanvas&) const override;
  animation::Phase Tick(time::Timer&) override;
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
