#pragma once

#include <vector>

#include "action.hh"
#include "math.hh"
#include "str.hh"
#include "time.hh"
#include "widget.hh"

namespace automat::animation {
struct Display;
}  // namespace automat::animation

namespace automat::gui {

struct Keyboard;
struct PointerImpl;
struct Widget;
struct Window;
struct DrawContext;

struct PointerMoveCallback {
  virtual void PointerMove(Pointer&, Vec2 position) = 0;
};

struct Pointer final {
  Pointer(Window&, Vec2 position);
  ~Pointer();
  void Move(Vec2 position);
  void Wheel(float delta);
  void ButtonDown(PointerButton);
  void ButtonUp(PointerButton);

  Widget* GetWidget() {
    if (action) {
      return action->Widget();
    }
    return nullptr;
  }

  enum IconType { kIconArrow, kIconHand, kIconIBeam };

  IconType Icon() const;
  void PushIcon(IconType);
  void PopIcon();

  Vec2 PositionWithin(const Widget&) const;
  Vec2 PositionWithinRootMachine() const;

  animation::Display& AnimationContext() const;

  maf::Str ToStr() const;

  Window& window;
  // The main keyboard associated with this pointer device. May be null!
  Keyboard* keyboard;

  Vec2 pointer_position;
  std::vector<Pointer::IconType> icons;

  Vec2 button_down_position[kButtonCount];
  time::SystemPoint button_down_time[kButtonCount];

  maf::Vec<PointerMoveCallback*> move_callbacks;

  std::unique_ptr<Action> action;
  Path path;
};

}  // namespace automat::gui
