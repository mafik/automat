#pragma once

#include "animation.hh"
#include "connection.hh"
#include "gui_button.hh"
#include "gui_constants.hh"
#include "widget.hh"

namespace automat {
struct Location;
struct Argument;
}  // namespace automat

namespace automat::gui {

struct ConnectionWidget;

struct DragConnectionAction : Action {
  Location& from;
  Argument& arg;
  animation::Context* animation_context;
  std::unique_ptr<ConnectionState> state;

  Vec2 current_position;
  DragConnectionAction(Location&, Argument&);
  ~DragConnectionAction() override;
  void Begin(gui::Pointer& pointer) override;
  void Update(gui::Pointer& pointer) override;
  void End() override;
  void DrawAction(DrawContext&) override;
};

struct ConnectionWidget : Button {
  Location& from;
  Argument& arg;

  ConnectionWidget(Location&, Argument&);

  void Draw(DrawContext&) const override;
  std::unique_ptr<Action> ButtonDownAction(Pointer&, PointerButton) override;
};

}  // namespace automat::gui