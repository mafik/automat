#pragma once

#include <include/core/SkCanvas.h>

#include <memory>
#include <vector>

#include "animation.h"
#include "math.h"

namespace automat::gui {

struct CaretImpl;
struct CaretOwner;
struct Keyboard;
struct KeyboardImpl;
struct Window;
struct Widget;
struct DrawContext;
using Path = std::vector<Widget*>;

enum class AnsiKey : uint8_t {
  Unknown,
  Escape,
  F1,
  F2,
  F3,
  F4,
  F5,
  F6,
  F7,
  F8,
  F9,
  F10,
  F11,
  F12,
  PrintScreen,
  ScrollLock,
  Pause,
  Insert,
  Delete,
  Home,
  End,
  PageUp,
  PageDown,
  Up,
  Down,
  Left,
  Right,
  NumLock,
  NumpadDivide,
  NumpadMultiply,
  NumpadMinus,
  NumpadPlus,
  NumpadEnter,
  NumpadPeriod,
  Numpad0,
  Numpad1,
  Numpad2,
  Numpad3,
  Numpad4,
  Numpad5,
  Numpad6,
  Numpad7,
  Numpad8,
  Numpad9,
  Grave,
  Digit1,
  Digit2,
  Digit3,
  Digit4,
  Digit5,
  Digit6,
  Digit7,
  Digit8,
  Digit9,
  Digit0,
  Minus,
  Equals,
  Backspace,
  Tab,
  Q,
  W,
  E,
  R,
  T,
  Y,
  U,
  I,
  O,
  P,
  BracketLeft,
  BracketRight,
  Backslash,
  CapsLock,
  A,
  S,
  D,
  F,
  G,
  H,
  J,
  K,
  L,
  Semicolon,
  Apostrophe,
  Enter,
  ShiftLeft,
  Z,
  X,
  C,
  V,
  B,
  N,
  M,
  Comma,
  Period,
  Slash,
  ShiftRight,
  ControlLeft,
  SuperLeft,
  AltLeft,
  Space,
  AltRight,
  SuperRight,
  Application,
  ControlRight,
  Count
};

struct Key {
  AnsiKey physical;
  AnsiKey logical;
  std::string text;
};

struct Caret final {
  Caret(CaretImpl& impl);
  void PlaceIBeam(Vec2 position);

 private:
  CaretImpl& impl;
};

struct CaretOwner {
  std::vector<CaretImpl*> carets;
  virtual ~CaretOwner();

  Caret& RequestCaret(Keyboard&, const Path& widget_path, Vec2 position);
  virtual void ReleaseCaret(Caret&) = 0;
  virtual Widget* CaretWidget() = 0;

  virtual void KeyDown(Caret&, Key);
  virtual void KeyUp(Caret&, Key);
};

struct Keyboard final {
  Keyboard(Window&);
  ~Keyboard();
  void Draw(DrawContext&) const;
  void KeyDown(Key);
  void KeyUp(Key);

 private:
  std::unique_ptr<KeyboardImpl> impl;
  friend CaretOwner;
  friend Window;
};

}  // namespace automat::gui
