// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "key.hh"

#include <map>

#include "str.hh"

namespace automat::gui {

StrView ToStr(AnsiKey k) noexcept {
  using enum AnsiKey;
  switch (k) {
    case Escape:
      return "Esc";
    case F1:
      return "F1";
    case F2:
      return "F2";
    case F3:
      return "F3";
    case F4:
      return "F4";
    case F5:
      return "F5";
    case F6:
      return "F6";
    case F7:
      return "F7";
    case F8:
      return "F8";
    case F9:
      return "F9";
    case F10:
      return "F10";
    case F11:
      return "F11";
    case F12:
      return "F12";
    case PrintScreen:
      return "PrintScreen";
    case ScrollLock:
      return "ScrollLock";
    case Pause:
      return "Pause";
    case Insert:
      return "Insert";
    case Delete:
      return "Delete";
    case Home:
      return "Home";
    case End:
      return "End";
    case PageUp:
      return "PageUp";
    case PageDown:
      return "PageDown";
    case Up:
      return "Up";
    case Down:
      return "Down";
    case Left:
      return "Left";
    case Right:
      return "Right";
    case NumLock:
      return "NumLock";
    case NumpadDivide:
      return "NumpadDivide";
    case NumpadMultiply:
      return "NumpadMultiply";
    case NumpadMinus:
      return "NumpadMinus";
    case NumpadPlus:
      return "NumpadPlus";
    case NumpadEnter:
      return "NumpadEnter";
    case NumpadPeriod:
      return "NumpadPeriod";
    case Numpad0:
      return "Numpad 0";
    case Numpad1:
      return "Numpad 1";
    case Numpad2:
      return "Numpad 2";
    case Numpad3:
      return "Numpad 3";
    case Numpad4:
      return "Numpad 4";
    case Numpad5:
      return "Numpad 5";
    case Numpad6:
      return "Numpad 6";
    case Numpad7:
      return "Numpad 7";
    case Numpad8:
      return "Numpad 8";
    case Numpad9:
      return "Numpad 9";
    case Grave:
      return "`";
    case Digit1:
      return "1";
    case Digit2:
      return "2";
    case Digit3:
      return "3";
    case Digit4:
      return "4";
    case Digit5:
      return "5";
    case Digit6:
      return "6";
    case Digit7:
      return "7";
    case Digit8:
      return "8";
    case Digit9:
      return "9";
    case Digit0:
      return "0";
    case Minus:
      return "-";
    case Equals:
      return "=";
    case Backspace:
      return "Backspace";
    case Tab:
      return "Tab";
    case Q:
      return "Q";
    case W:
      return "W";
    case E:
      return "E";
    case R:
      return "R";
    case T:
      return "T";
    case Y:
      return "Y";
    case U:
      return "U";
    case I:
      return "I";
    case O:
      return "O";
    case P:
      return "P";
    case BracketLeft:
      return "[";
    case BracketRight:
      return "]";
    case Backslash:
      return "\\";
    case CapsLock:
      return "CapsLock";
    case A:
      return "A";
    case S:
      return "S";
    case D:
      return "D";
    case F:
      return "F";
    case G:
      return "G";
    case H:
      return "H";
    case J:
      return "J";
    case K:
      return "K";
    case L:
      return "L";
    case Semicolon:
      return ";";
    case Apostrophe:
      return "'";
    case Enter:
      return "Enter";
    case ShiftLeft:
      return "Left Shift";
    case Z:
      return "Z";
    case X:
      return "X";
    case C:
      return "C";
    case V:
      return "V";
    case B:
      return "B";
    case N:
      return "N";
    case M:
      return "M";
    case Comma:
      return ".";
    case Period:
      return ",";
    case Slash:
      return "Slash";
    case ShiftRight:
      return "Right Shift";
    case ControlLeft:
      return "Left Control";
    case SuperLeft:
      return "Left Super";
    case AltLeft:
      return "Left Alt";
    case Space:
      return "Space";
    case AltRight:
      return "Right Alt";
    case SuperRight:
      return "Right Super";
    case Application:
      return "Application";
    case ControlRight:
      return "Right Control";
    default:
      return "<?>";
  }
}

AnsiKey AnsiKeyFromStr(StrView str) noexcept {
  static std::map<StrView, AnsiKey> map = []() {
    std::map<StrView, AnsiKey> map;
    for (AnsiKey key = (AnsiKey)0; key < AnsiKey::Count; key = (AnsiKey)((int)key + 1)) {
      map[ToStr(key)] = key;
    }
    return map;
  }();
  if (auto it = map.find(str); it != map.end()) {
    return it->second;
  }
  return AnsiKey::Unknown;
}

}  // namespace automat::gui