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

struct ConnectionWidget : ArgumentToy {
  Optional<Vec2> manual_position;  // position of the plug (bottom center)

  // Updated in `Tick()`
  SkColor tint;
  SkPath from_shape;  // board coords
  SkPath to_shape;    // board coords
  animation::Approach<> cable_width;
  Vec<Vec2AndDir> to_points;
  float transparency = 1;
  float alpha = 0;
  bool hidden = false;
  Optional<ArcLine> arcline;        // routed cable for non-physical connections
  Optional<Vec2> end_anchor_local;  // cable end in the end widget's local frame
  Str refusal_text;
  time::SteadyPoint refusal_until = {};

  ConnectionWidget(Widget* parent, Object&, Argument::Table&);

  StrView Name() const override { return "ConnectionWidget"; }
  void ShowRefusal(Str text);
  SkPath Shape() const override;
  Tock Tick(time::Timer&) override;
  void Draw(SkCanvas&) const override;
  Compositor GetCompositor() const override { return Compositor::ANCHOR_WARP; }
  std::unique_ptr<Action> FindAction(Pointer&, ActionTrigger) override;
  Optional<Rect> DrawBounds() const override;
  Vec<Vec2> TextureAnchors() override;
};

struct CableWidget : ConnectionWidget {
  Optional<CablePhysicsSimulation>
      state;  // if the state is non-empty then the cable is physically simulated
  std::unique_ptr<ui::Widget> icon;

  using ConnectionWidget::ConnectionWidget;

  StrView Name() const override { return "CableWidget"; }
  SkPath Shape() const override;
  Tock Tick(time::Timer&) override;
  void Draw(SkCanvas&) const override;
  Optional<Rect> DrawBounds() const override;
};

struct RoutedCableWidget : ConnectionWidget {
  float length = 0;

  using ConnectionWidget::ConnectionWidget;

  StrView Name() const override { return "RoutedCableWidget"; }
  Tock Tick(time::Timer&) override;
  void Draw(SkCanvas&) const override;
};

struct StreamPipeWidget : ConnectionWidget {
  RateEstimator byte_rate;
  RateEstimator unit_rate;
  float dash_phase = 0;
  float bytes_per_s = 0;
  float units_per_s = 0;
  float rate_drawn = 0;
  uint64_t fill = 0;
  uint64_t capacity = 0;
  StrView fill_unit = {};
  float fill_drawn = 0;
  StreamBlocked blocked = StreamBlocked::None;
  float blocked_score = 0;
  bool blocked_shown = false;
  Str format;
  float length = 0;

  using ConnectionWidget::ConnectionWidget;

  StrView Name() const override { return "StreamPipeWidget"; }
  SkPath Shape() const override;
  Tock Tick(time::Timer&) override;
  void Draw(SkCanvas&) const override;
  Optional<Rect> DrawBounds() const override;
};

struct SpotlightWidget : ArgumentToy {
  SpotlightWidget(Widget* parent, Object&, Argument::Table&);

  StrView Name() const override { return "SpotlightWidget"; }
  SkPath Shape() const override { return SkPath(); }
  Tock Tick(time::Timer&) override;
  void Draw(SkCanvas&) const override;
  Optional<Rect> DrawBounds() const override;
};

struct InvisibleWidget : ArgumentToy {
  InvisibleWidget(Widget* parent, Object&, Argument::Table&);

  StrView Name() const override { return "InvisibleWidget"; }
  SkPath Shape() const override { return SkPath(); }
  Tock Tick(time::Timer&) override;
};

// Now that ConnectionWidget is defined, we can check whether Argument can make toys
static_assert(ToyMaker<Argument>);

void DrawArrow(SkCanvas& canvas, const SkPath& from_shape, const SkPath& to_shape);

}  // namespace automat::ui
