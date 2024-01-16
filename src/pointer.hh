#pragma once

#include <memory>
#include <vector>

#include "math.hh"

namespace automat::animation {
struct Context;
}  // namespace automat::animation

namespace automat::gui {

struct Keyboard;
struct PointerImpl;
struct Widget;
struct Window;

using Path = std::vector<Widget*>;

enum PointerButton { kButtonUnknown, kMouseLeft, kMouseMiddle, kMouseRight, kButtonCount };

struct Pointer final {
  Pointer(Window&, Vec2 position);
  ~Pointer();
  void Move(Vec2 position);
  void Wheel(float delta);
  void ButtonDown(PointerButton);
  void ButtonUp(PointerButton);

  enum IconType { kIconArrow, kIconHand, kIconIBeam };

  IconType Icon() const;
  void PushIcon(IconType);
  void PopIcon();

  const Path& Path() const;
  Vec2 PositionWithin(Widget&) const;
  Vec2 PositionWithinRootMachine() const;

  Keyboard& Keyboard();

  animation::Context& AnimationContext() const;

 private:
  std::unique_ptr<PointerImpl> impl;
  friend struct Window;
};

}  // namespace automat::gui
