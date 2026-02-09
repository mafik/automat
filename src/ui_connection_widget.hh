// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "animation.hh"
#include "argument.hh"
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
  bool Highlight(Object&, Atom&) const override;
};

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
  mutable animation::Approach<> cable_width;
  Vec<Vec2AndDir> to_points;
  float transparency = 1;
  float alpha = 0;
  float length = 0;
  mutable std::unique_ptr<Object::Toy> prototype_widget;

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

  // TODO: Needs a function that will update the `parent` ptr.
  // - called in ConnectionDragAction
  // - called in ~ConnectionDragAction / RootMachine::ConnectAtPoint
  // - called in
};

// Now that ConnectionWidget is defined, we can check whether ArgumentOf can make toys
static_assert(ToyMaker<ArgumentOf>);

void DrawArrow(SkCanvas& canvas, const SkPath& from_shape, const SkPath& to_shape);

struct ConnectionWidgetRange {
  const Object* obj;
  const Argument* arg;

  using MapType = std::map<ToyStore::Key, std::unique_ptr<Toy>>;
  struct end_iterator {};

  struct iterator {
    const Object* obj;
    const Argument* arg;
    MapType::iterator it;
    MapType::iterator end_it;

    iterator(const Object* obj, const Argument* arg, MapType::iterator it, MapType::iterator end_it)
        : obj(obj), arg(arg), it(it), end_it(end_it) {
      Advance();
    }

    void Advance() {
      while (it != end_it) {
        auto* w = dynamic_cast<ConnectionWidget*>(it->second.get());
        if (w && w->start_weak.OwnerUnsafe<Object>() == obj &&
            (arg == nullptr || w->start_weak.GetUnsafe() == arg)) {
          return;
        }
        ++it;
      }
    }

    iterator& operator++() {
      ++it;
      Advance();
      return *this;
    }

    bool operator==(const end_iterator&) const { return it == end_it; }

    ConnectionWidget& operator*() const { return static_cast<ConnectionWidget&>(*it->second); }
  };

  ConnectionWidgetRange(const Object* obj, const Argument* arg) : obj(obj), arg(arg) {}

  iterator begin() const {
    auto& container = ui::root_widget->toys.container;
    return iterator{obj, arg, container.begin(), container.end()};
  }

  end_iterator end() const { return end_iterator{}; }
};

}  // namespace automat::ui
