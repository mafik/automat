#pragma once

#include <vector>

#include "action.h"
#include "pointer.h"
#include "time.h"
#include "widget.h"


namespace automaton::gui {

struct Window;
struct WindowImpl;

struct PointerImpl {
  WindowImpl &window;
  Pointer &facade;
  vec2 pointer_position;
  std::vector<Pointer::IconType> icons;

  vec2 button_down_position[kButtonCount];
  time::point button_down_time[kButtonCount];

  std::unique_ptr<Action> action;
  Widget *hovered_widget = nullptr;
  SkMatrix hovered_widget_transform;

  PointerImpl(WindowImpl &window, Pointer &facade, vec2 position);
  ~PointerImpl();
  void Move(vec2 position);
  void Wheel(float delta);
  void ButtonDown(Button btn);
  void ButtonUp(Button btn);
  void Draw(SkCanvas &canvas, animation::State &animation_state) {
    if (action) {
      action->Draw(canvas, animation_state);
    }
  }
  Pointer::IconType Icon() const {
    if (icons.empty()) {
      return Pointer::kIconArrow;
    }
    return icons.back();
  }
  void PushIcon(Pointer::IconType new_icon) { icons.push_back(new_icon); }
  void PopIcon() { icons.pop_back(); }
  vec2 PositionWithin(Widget &) const;
};

} // namespace automaton::gui
