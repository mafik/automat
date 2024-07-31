#pragma once

#include "animation.hh"
#include "connection.hh"
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

  animation::PerDisplay<AnimationState> animation_state;

  mutable animation::Approach<> cable_width;
  mutable maf::Optional<OpticalConnectorState> state;
  mutable maf::Optional<Vec2> manual_position;  // position of the plug (bottom center)

  ConnectionWidget(Location&, Argument&);

  maf::StrView Name() const override { return "ConnectionWidget"; }
  SkPath Shape(animation::Display*) const override;
  void PreDraw(DrawContext&) const override;
  void Draw(DrawContext&) const override;
  std::unique_ptr<Action> ButtonDownAction(Pointer&, PointerButton) override;
};

void DrawArrow(SkCanvas& canvas, const SkPath& from_shape, const SkPath& to_shape);

}  // namespace automat::gui