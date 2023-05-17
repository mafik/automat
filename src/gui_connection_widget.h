#pragma once

#include "gui_button.h"
#include "gui_constants.h"
#include "location.h"
#include "widget.h"

namespace automaton::gui {

struct ConnectionWidget;

struct DragConnectionAction : Action {
  ConnectionWidget *widget;
  vec2 current_position;
  DragConnectionAction(ConnectionWidget *widget);
  ~DragConnectionAction() override;
  void Begin(gui::Pointer &pointer) override;
  void Update(gui::Pointer &pointer) override;
  void End() override;
  void Draw(SkCanvas &canvas, animation::State &animation_state) override;
};

struct ConnectionWidget : Button {
  Location *from;
  std::string label;
  DragConnectionAction *drag_action = nullptr;

  ConnectionWidget(Location *from, std::string_view label);
  Widget *ParentWidget() override;
  
  vec2 Position() const override;
  void Draw(SkCanvas &, animation::State &) const override;
  std::unique_ptr<Action> ButtonDownAction(Pointer &, PointerButton) override;

  vec2 Center() const;
};

} // namespace automaton::gui