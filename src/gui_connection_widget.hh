#pragma once

#include "gui_button.h"
#include "gui_constants.h"
#include "widget.h"

namespace automat {
struct Location;
}  // namespace automat

namespace automat::gui {

struct ConnectionWidget;

struct DragConnectionAction : Action {
  ConnectionWidget* widget;
  Vec2 current_position;
  DragConnectionAction(ConnectionWidget* widget);
  ~DragConnectionAction() override;
  void Begin(gui::Pointer& pointer) override;
  void Update(gui::Pointer& pointer) override;
  void End() override;
  void DrawAction(DrawContext&) override;
};

struct ConnectionWidget : Button {
  Location* from;
  std::string label;
  DragConnectionAction* drag_action = nullptr;

  ConnectionWidget(Location* from, std::string_view label);

  void Draw(DrawContext&) const override;
  std::unique_ptr<Action> ButtonDownAction(Pointer&, PointerButton) override;
};

}  // namespace automat::gui