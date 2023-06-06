#pragma once

#include <map>
#include <set>

#include "keyboard.h"
#include "product_ptr.h"
#include "window_impl.h"

namespace automat::gui {

struct KeyboardImpl;

struct CaretImpl {
  Caret facade;
  CaretOwner* owner = nullptr;
  SkPath shape;
  Path widget_path;
  time::point last_blink;
  KeyboardImpl& keyboard;
  CaretImpl(KeyboardImpl& keyboard);
  ~CaretImpl();
  void PlaceIBeam(vec2 position);
  SkPath MakeRootShape() const;
};

struct CaretAnimation {
  const KeyboardImpl& keyboard;
  animation::DeltaFraction delta_fraction;
  SkPath shape;
  time::point last_blink;
  animation::Approach fade_out;
  CaretAnimation(const KeyboardImpl&);
};

struct KeyboardAnimation {
  std::map<CaretImpl*, CaretAnimation> carets = {};
};

struct KeyboardImpl {
  WindowImpl& window;
  Keyboard& facade;
  PointerImpl* pointer = nullptr;
  std::set<std::unique_ptr<CaretImpl>> carets;
  std::bitset<static_cast<size_t>(AnsiKey::Count)> pressed_keys;
  mutable product_ptr<KeyboardAnimation> anim;
  KeyboardImpl(WindowImpl& window, Keyboard& facade);
  ~KeyboardImpl();
  void Draw(DrawContext&) const;
  void KeyDown(Key);
  void KeyUp(Key);
};

}  // namespace automat::gui
