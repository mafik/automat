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
  animation::Display* display;
  std::unique_ptr<audio::Effect> effect;

  Vec2 grab_offset;
  DragConnectionAction(Pointer&, ConnectionWidget&);
  ~DragConnectionAction() override;
  void Begin() override;
  void Update() override;
  void End() override;
};

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

  mutable animation::Approach<> cable_width;
  mutable maf::Optional<OpticalConnectorState>
      state;  // if the state is non-empty then the cable is physically simulated
  mutable float transparency = 1;
  mutable float length = 0;
  mutable maf::Optional<Vec2> manual_position;  // position of the plug (bottom center)

  ConnectionWidget(Location&, Argument&);

  maf::StrView Name() const override { return "ConnectionWidget"; }
  SkPath Shape() const override;
  animation::Phase PreDraw(DrawContext&) const override;
  animation::Phase Draw(DrawContext&) const override;
  std::unique_ptr<Action> FindAction(Pointer&, ActionTrigger) override;
  maf::Optional<Rect> TextureBounds() const override;
};

void DrawArrow(SkCanvas& canvas, const SkPath& from_shape, const SkPath& to_shape);

}  // namespace automat::gui