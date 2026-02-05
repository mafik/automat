// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "animation.hh"
#include "audio.hh"
#include "connector_optical.hh"
#include "object.hh"
#include "ptr.hh"
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
  bool Highlight(Object&, Part&) const override;
  ui::Widget* Widget() override;
};

void UnparentConnectionWidget(ConnectionWidget&);
void ReparentConnectionWidget(ConnectionWidget&, Widget& new_parent);

// ConnectionWidget can function in three different modes, depending on how the argument is set to
// draw:
// - Arrow: a simple arrow pointing to the target location
// - Physical Cable: a cable with a plug at the end that wiggles when moved
// - Analytically-routed Cable: a cable that always follows the nicest path
//
// TODO: separate the state of these three modes better
struct ConnectionWidget : Toy {
  NestedWeakPtr<Argument> start_weak;

  struct AnimationState {
    float radar_alpha = 0;
    float radar_alpha_target = 0;
    float prototype_alpha = 0;
    float prototype_alpha_target = 0;
    double time_seconds = 0;
  };

  static ConnectionWidget* FindOrNull(Object&, Argument&);
  static ConnectionWidget* FindOrNull(const NestedWeakPtr<Argument>& ptr) {
    return FindOrNull(*ptr.OwnerUnsafe<Object>(), *ptr.GetUnsafe());
  }

  mutable AnimationState animation_state;

  Optional<CablePhysicsSimulation>
      state;  // if the state is non-empty then the cable is physically simulated
  Optional<Vec2> manual_position;  // position of the plug (bottom center)

  // Updated in `Tick()`
  Argument::Style style;
  Vec2AndDir pos_dir;  // position of connection start
  SkPath from_shape;   // machine coords
  SkPath to_shape;     // machine coords
  SkPath clip;         // machine coords
  mutable animation::Approach<> cable_width;
  Vec<Vec2AndDir> to_points;
  float transparency = 1;
  float alpha = 0;
  float length = 0;
  mutable std::unique_ptr<Toy> prototype_widget;

  ConnectionWidget(Widget* parent, Object&, Argument&);

  // Helper to get the Location and Argument from start_weak
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
  void FromMoved();

  float GetBaseScale() const override { return 1.f; }

  // Places where the connections to this widget may terminate.
  // Local (metric) coordinates.
  void ConnectionPositions(Vec<Vec2AndDir>&) const override {}

  // TODO: Needs a function that will update the `parent` ptr.
  // - called in ConnectionDragAction
  // - called in ~ConnectionDragAction / RootMachine::ConnectAtPoint
  // - called in
};

void DrawArrow(SkCanvas& canvas, const SkPath& from_shape, const SkPath& to_shape);

struct ConnectionWidgetRange {
  const Object* obj;
  const Argument* arg;

  struct end_iterator {};

  struct iterator {
    const Object* obj;
    const Argument* arg;
    int i;

    iterator(const Object* obj, const Argument* arg, int i) : obj(obj), arg(arg), i(i) {
      Advance();
    }

    // Function that exits ONLY when the iterator is pointing at a valid connection widget or end of
    // range.
    void Advance() {
      auto& widgets = ui::root_widget->connection_widgets;
      auto size = widgets.size();
      while (i < size) {
        ConnectionWidget& w = *widgets[i];
        if (w.start_weak.OwnerUnsafe<Object>() == obj &&
            (arg == nullptr || w.start_weak.GetUnsafe() == arg)) {
          return;
        }
        ++i;
      }
    }

    iterator& operator++() {
      ++i;
      Advance();
      return *this;
    }

    bool operator==(const end_iterator&) const {
      return i == ui::root_widget->connection_widgets.size();
    }

    ConnectionWidget& operator*() const { return *ui::root_widget->connection_widgets[i]; }
  };

  ConnectionWidgetRange(const Object* obj, const Argument* arg) : obj(obj), arg(arg) {}

  iterator begin() const { return iterator{obj, arg, 0}; }

  end_iterator end() const { return end_iterator{}; }
};

}  // namespace automat::ui
