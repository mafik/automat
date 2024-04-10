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
  animation::Context* animation_context;

  Vec2 grab_offset;
  DragConnectionAction(ConnectionWidget&);
  ~DragConnectionAction() override;
  void Begin(gui::Pointer& pointer) override;
  void Update(gui::Pointer& pointer) override;
  void End() override;
  void DrawAction(DrawContext&) override {}
};

struct ConnectionWidget : Widget {
  Location& from;
  Argument& arg;
  mutable OpticalConnectorState state;
  mutable maf::Optional<Vec2> manual_position;  // position of the plug (bottom center)

  ConnectionWidget(Location&, Argument&);

  maf::StrView Name() const override { return "ConnectionWidget"; }
  SkPath Shape() const override;
  void Draw(DrawContext&) const override;
  std::unique_ptr<Action> ButtonDownAction(Pointer&, PointerButton) override;
};

}  // namespace automat::gui