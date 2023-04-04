#pragma once

#include "keyboard.h"
#include "product_ptr.h"
#include "window_impl.h"

namespace automaton::gui {

struct KeyboardImpl;

struct CaretAnimationState {
  SkPath shape;
};

struct CaretImpl {
  Caret facade;
  CaretOwner* owner;
  SkPath shape;
  product_ptr<CaretAnimationState> animation_state;
  KeyboardImpl &keyboard;
  CaretImpl(KeyboardImpl &keyboard);
  ~CaretImpl();
  void PlaceIBeam(vec2 canvas_position);
  void Draw(SkCanvas &, animation::State &animation_state) const;
};

struct KeyboardImpl {
  WindowImpl &window;
  Keyboard &facade;
  std::vector<std::unique_ptr<CaretImpl>> carets;
  std::bitset<static_cast<size_t>(AnsiKey::Count)> pressed_keys;
  KeyboardImpl(WindowImpl &window, Keyboard &facade);
  ~KeyboardImpl();
  void Draw(SkCanvas &, animation::State &animation_state) const;
  void KeyDown(Key);
  void KeyUp(Key);
};

} // namespace automaton::gui
