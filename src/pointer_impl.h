#pragma once

#include <vector>

#include "action.h"
#include "pointer.h"
#include "time.h"
#include "widget.h"

namespace automat::gui {

struct Window;
struct WindowImpl;

struct PointerImpl {
  WindowImpl& window;
  Pointer& facade;
  Vec2 pointer_position;
  std::vector<Pointer::IconType> icons;

  Vec2 button_down_position[kButtonCount];
  time::point button_down_time[kButtonCount];

  std::unique_ptr<Action> action;
  Path path;

  KeyboardImpl* keyboard = nullptr;

  PointerImpl(WindowImpl& window, Pointer& facade, Vec2 position);
  ~PointerImpl();
  void Move(Vec2 position);
  void Wheel(float delta);
  void ButtonDown(PointerButton btn);
  void ButtonUp(PointerButton btn);
  void Draw(DrawContext& ctx) {
    if (action) {
      action->DrawAction(ctx);
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
  Vec2 PositionWithin(Widget&) const;
  Vec2 PositionWithinRootMachine() const;
  Keyboard& Keyboard();
};

}  // namespace automat::gui
