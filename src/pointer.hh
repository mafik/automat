#pragma once

#include <vector>

#include "action.hh"
#include "math.hh"
#include "time.hh"

namespace automat::animation {
struct Display;
}  // namespace automat::animation

namespace automat::gui {

struct Keyboard;
struct PointerImpl;
struct Widget;
struct Window;
struct DrawContext;

using Path = std::vector<Widget*>;

template <typename T>
T* Closest(const Path& path) {
  for (int i = path.size() - 1; i >= 0; --i) {
    if (auto* result = dynamic_cast<T*>(path[i])) {
      return result;
    }
  }
  return nullptr;
}

enum PointerButton { kButtonUnknown, kMouseLeft, kMouseMiddle, kMouseRight, kButtonCount };

struct Pointer final {
  Pointer(Window&, Vec2 position);
  ~Pointer();
  void Move(Vec2 position);
  void Wheel(float delta);
  void ButtonDown(PointerButton);
  void ButtonUp(PointerButton);

  void Draw(DrawContext& ctx);

  enum IconType { kIconArrow, kIconHand, kIconIBeam };

  IconType Icon() const;
  void PushIcon(IconType);
  void PopIcon();

  Vec2 PositionWithin(Widget&) const;
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

  std::unique_ptr<Action> action;
  Path path;
};

}  // namespace automat::gui
