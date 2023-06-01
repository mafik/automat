#pragma once

#include <memory>
#include <vector>

#include "math.h"

namespace automat::gui {

struct Keyboard;
struct PointerImpl;
struct Widget;
struct Window;

using Path = std::vector<Widget *>;

enum PointerButton {
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
  void ButtonDown(PointerButton);
  void ButtonUp(PointerButton);

  enum IconType { kIconArrow, kIconHand, kIconIBeam };

  IconType Icon() const;
  void PushIcon(IconType);
  void PopIcon();

  const Path &Path() const;
  vec2 PositionWithin(Widget &) const;
  vec2 PositionWithinRootMachine() const;

  Keyboard &Keyboard();

private:
  std::unique_ptr<PointerImpl> impl;
  friend struct Window;
};

} // namespace automat::gui
