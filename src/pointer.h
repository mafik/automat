#pragma once

#include "math.h"

namespace automaton::gui {

struct Keyboard;
struct PointerImpl;
struct Widget;
struct Window;

enum Button {
  kButtonUnknown,
  kMouseLeft,
  kMouseMiddle,
  kMouseRight,
  kButtonCount
};

struct Pointer final {
  Pointer(Window &, vec2 position);
  ~Pointer();
  void Move(vec2 position);
  void Wheel(float delta);
  void ButtonDown(Button);
  void ButtonUp(Button);

  enum IconType { kIconArrow, kIconHand, kIconIBeam };

  IconType Icon() const;
  void PushIcon(IconType);
  void PopIcon();

  vec2 PositionWithin(Widget &) const;

  Keyboard &Keyboard();

private:
  std::unique_ptr<PointerImpl> impl;
  friend struct Window;
};

} // namespace automaton::gui
