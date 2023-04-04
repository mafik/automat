#pragma once

#include <memory>

#include <include/core/SkCanvas.h>

#include "animation.h"
#include "math.h"

namespace automaton::gui {

struct CaretImpl;
struct Keyboard;
struct KeyboardImpl;
struct Window;

enum Key { kKeyUnknown, kKeyW, kKeyA, kKeyS, kKeyD, kKeyCount };

struct Caret final {
  Caret(Keyboard &);
  ~Caret();
  void PlaceIBeam(vec2 canvas_position);

private:
  std::unique_ptr<CaretImpl> impl;
};

struct Keyboard final {
  Keyboard(Window &);
  ~Keyboard();
  void Draw(SkCanvas &, animation::State &animation_state) const;
  void KeyDown(Key);
  void KeyUp(Key);

private:
  std::unique_ptr<KeyboardImpl> impl;
  friend Caret;
  friend Window;
};

} // namespace automaton::gui
