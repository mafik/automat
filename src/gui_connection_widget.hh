#pragma once

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
  std::string label;
  Vec2 current_position;
  DragConnectionAction(Location&, std::string label);
  ~DragConnectionAction() override;
  void Begin(gui::Pointer& pointer) override;
  void Update(gui::Pointer& pointer) override;
  void End() override;
  void DrawAction(DrawContext&) override;
};

struct ConnectionWidget : Button {
  Location* from;
  std::string label;

  ConnectionWidget(Location* from, std::string_view label);

  void Draw(DrawContext&) const override;
  std::unique_ptr<Action> ButtonDownAction(Pointer&, PointerButton) override;
};

}  // namespace automat::gui