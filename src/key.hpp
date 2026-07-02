#pragma once
// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT

#include <cstdint>

#include "str.hpp"

namespace automat::ui {

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

StrView ToStr(AnsiKey) noexcept;
AnsiKey AnsiKeyFromStr(StrView) noexcept;

struct Key {
  // The effective modifier state (held, latched or locked). Loosely based on XKB.
  bool ctrl;
  bool alt;
  bool shift;
  bool windows;  // Super, the OS key
  bool caps_lock;
  bool num_lock;
  bool alt_gr;         // ISO_Level3_Shift, picks shift levels 3 & 4
  bool level5;         // ISO_Level5_Shift, picks shift levels 5+
  uint8_t layout = 0;  // active keyboard group (XKB layout index)
  AnsiKey physical;
  AnsiKey logical;
  std::string text;
};

}  // namespace automat::ui