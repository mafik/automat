// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "win_key.hh"

#include <windows.h>

#include "format.hh"
#include "log.hh"

using automat::ui::AnsiKey;
using namespace automat;

// Source:
// https://handmade.network/forums/t/2011-keyboard_inputs_-_scancodes,_raw_input,_text_input,_key_names
enum class Scancode {
  Unknown = 0x00,
  Escape = 0x01,
  Digit1 = 0x02,
  Digit2 = 0x03,
  Digit3 = 0x04,
  Digit4 = 0x05,
  Digit5 = 0x06,
  Digit6 = 0x07,
  Digit7 = 0x08,
  Digit8 = 0x09,
  Digit9 = 0x0A,
  Digit0 = 0x0B,
  Minus = 0x0C,
  Equals = 0x0D,
  Backspace = 0x0E,
  Tab = 0x0F,
  Q = 0x10,
  W = 0x11,
  E = 0x12,
  R = 0x13,
  T = 0x14,
  Y = 0x15,
  U = 0x16,
  I = 0x17,
  O = 0x18,
  P = 0x19,
  BracketLeft = 0x1A,
  BracketRight = 0x1B,
  Enter = 0x1C,
  ControlLeft = 0x1D,
  A = 0x1E,
  S = 0x1F,
  D = 0x20,
  F = 0x21,
  G = 0x22,
  H = 0x23,
  J = 0x24,
  K = 0x25,
  L = 0x26,
  Semicolon = 0x27,
  Apostrophe = 0x28,
  Grave = 0x29,
  ShiftLeft = 0x2A,
  Backslash = 0x2B,
  Z = 0x2C,
  X = 0x2D,
  C = 0x2E,
  V = 0x2F,
  B = 0x30,
  N = 0x31,
  M = 0x32,
  Comma = 0x33,
  Period = 0x34,
  Slash = 0x35,
  ShiftRight = 0x36,
  NumpadMultiply = 0x37,
  AltLeft = 0x38,
  Space = 0x39,
  CapsLock = 0x3A,
  F1 = 0x3B,
  F2 = 0x3C,
  F3 = 0x3D,
  F4 = 0x3E,
  F5 = 0x3F,
  F6 = 0x40,
  F7 = 0x41,
  F8 = 0x42,
  F9 = 0x43,
  F10 = 0x44,
  NumLock = 0x45,
  ScrollLock = 0x46,
  Numpad7 = 0x47,
  Numpad8 = 0x48,
  Numpad9 = 0x49,
  NumpadMinus = 0x4A,
  Numpad4 = 0x4B,
  Numpad5 = 0x4C,
  Numpad6 = 0x4D,
  NumpadPlus = 0x4E,
  Numpad1 = 0x4F,
  Numpad2 = 0x50,
  Numpad3 = 0x51,
  Numpad0 = 0x52,
  NumpadPeriod = 0x53,
  Alt_printScreen = 0x54, /* Alt + print screen. MapVirtualKeyEx( VK_SNAPSHOT,
                             MAPVK_VK_TO_VSC_EX, 0 ) returns scancode 0x54. */
  BracketAngle = 0x56,    /* Key between the left shift and Z. */
  F11 = 0x57,
  F12 = 0x58,
  Oem1 = 0x5a, /* VK_OEM_WSCTRL */
  Oem2 = 0x5b, /* VK_OEM_FINISH */
  Oem3 = 0x5c, /* VK_OEM_JUMP */
  OraseEOF = 0x5d,
  Oem4 = 0x5e, /* VK_OEM_BACKTAB */
  Oem5 = 0x5f, /* VK_OEM_AUTO */
  Zoom = 0x62,
  Help = 0x63,
  F13 = 0x64,
  F14 = 0x65,
  F15 = 0x66,
  F16 = 0x67,
  F17 = 0x68,
  F18 = 0x69,
  F19 = 0x6a,
  F20 = 0x6b,
  F21 = 0x6c,
  F22 = 0x6d,
  F23 = 0x6e,
  Oem6 = 0x6f, /* VK_OEM_PA3 */
  Katakana = 0x70,
  Oem7 = 0x71, /* VK_OEM_RESET */
  F24 = 0x76,
  Sbcschar = 0x77,
  Convert = 0x79,
  Nonconvert = 0x7B, /* VK_OEM_PA1 */

  MediaPrevious = 0xE010,
  MediaNext = 0xE019,
  NumpadEnter = 0xE01C,
  ControlRight = 0xE01D,
  VolumeMute = 0xE020,
  LaunchApp2 = 0xE021,
  MediaPlay = 0xE022,
  MediaStop = 0xE024,
  VolumeDown = 0xE02E,
  VolumeUp = 0xE030,
  BrowserHome = 0xE032,
  NumpadDivide = 0xE035,
  PrintScreen = 0xE037,
  /*
  sc_printScreen:
  - make: 0xE02A 0xE037
  - break: 0xE0B7 0xE0AA
  - MapVirtualKeyEx( VK_SNAPSHOT, MAPVK_VK_TO_VSC_EX, 0 ) returns scancode 0x54;
  - There is no VK_KEYDOWN with VK_SNAPSHOT.
  */
  AltRight = 0xE038,
  Cancel = 0xE046, /* CTRL + Pause */
  Home = 0xE047,
  ArrowUp = 0xE048,
  PageUp = 0xE049,
  ArrowLeft = 0xE04B,
  ArrowRight = 0xE04D,
  End = 0xE04F,
  ArrowDown = 0xE050,
  PageDown = 0xE051,
  Insert = 0xE052,
  Delete = 0xE053,
  MetaLeft = 0xE05B,
  MetaRight = 0xE05C,
  Application = 0xE05D,
  Power = 0xE05E,
  Sleep = 0xE05F,
  Wake = 0xE063,
  BrowserSearch = 0xE065,
  BrowserFavorites = 0xE066,
  BrowserRefresh = 0xE067,
  BrowserStop = 0xE068,
  BrowserForward = 0xE069,
  BrowserBack = 0xE06A,
  LaunchApp1 = 0xE06B,
  LaunchEmail = 0xE06C,
  LaunchMedia = 0xE06D,

  Pause = 0xE11D45,
  /*
  Pause:
  - make: 0xE11D 45 0xE19D C5
  - make in raw input: 0xE11D 0x45
  - break: none
  - No repeat when you hold the key down
  - There are no break so I don't know how the key down/up is expected to work.
  Raw input sends "keydown" and "keyup" messages, and it appears that the keyup
  message is sent directly after the keydown message (you can't hold the key
  down) so depending on when GetMessage or PeekMessage will return messages, you
  may get both a keydown and keyup message "at the same time". If you use VK
  messages most of the time you only get keydown messages, but some times you
  get keyup messages too.
  - when pressed at the same time as one or both control keys, generates a
  0xE046 (sc_cancel) and the string for that scancode is "break".
  */
};

AnsiKey ScanCodeToKey(uint32_t scan_code) {
  using enum AnsiKey;
  switch (Scancode(scan_code)) {
    case Scancode::Unknown:
      return Unknown;
    case Scancode::Escape:
      return Escape;
    case Scancode::F1:
      return F1;
    case Scancode::F2:
      return F2;
    case Scancode::F3:
      return F3;
    case Scancode::F4:
      return F4;
    case Scancode::F5:
      return F5;
    case Scancode::F6:
      return F6;
    case Scancode::F7:
      return F7;
    case Scancode::F8:
      return F8;
    case Scancode::F9:
      return F9;
    case Scancode::F10:
      return F10;
    case Scancode::F11:
      return F11;
    case Scancode::F12:
      return F12;
    case Scancode::PrintScreen:
      return PrintScreen;
    case Scancode::ScrollLock:
      return ScrollLock;
    case Scancode::Pause:
      return Pause;
    case Scancode::Insert:
      return Insert;
    case Scancode::Delete:
      return Delete;
    case Scancode::Home:
      return Home;
    case Scancode::End:
      return End;
    case Scancode::PageUp:
      return PageUp;
    case Scancode::PageDown:
      return PageDown;
    case Scancode::ArrowUp:
      return Up;
    case Scancode::ArrowDown:
      return Down;
    case Scancode::ArrowLeft:
      return Left;
    case Scancode::ArrowRight:
      return Right;
    case Scancode::NumLock:
      return NumLock;
    case Scancode::NumpadDivide:
      return NumpadDivide;
    case Scancode::NumpadMultiply:
      return NumpadMultiply;
    case Scancode::NumpadMinus:
      return NumpadMinus;
    case Scancode::NumpadPlus:
      return NumpadPlus;
    case Scancode::NumpadEnter:
      return NumpadEnter;
    case Scancode::NumpadPeriod:
      return NumpadPeriod;
    case Scancode::Numpad0:
      return Numpad0;
    case Scancode::Numpad1:
      return Numpad1;
    case Scancode::Numpad2:
      return Numpad2;
    case Scancode::Numpad3:
      return Numpad3;
    case Scancode::Numpad4:
      return Numpad4;
    case Scancode::Numpad5:
      return Numpad5;
    case Scancode::Numpad6:
      return Numpad6;
    case Scancode::Numpad7:
      return Numpad7;
    case Scancode::Numpad8:
      return Numpad8;
    case Scancode::Numpad9:
      return Numpad9;
    case Scancode::Grave:
      return Grave;
    case Scancode::Digit1:
      return Digit1;
    case Scancode::Digit2:
      return Digit2;
    case Scancode::Digit3:
      return Digit3;
    case Scancode::Digit4:
      return Digit4;
    case Scancode::Digit5:
      return Digit5;
    case Scancode::Digit6:
      return Digit6;
    case Scancode::Digit7:
      return Digit7;
    case Scancode::Digit8:
      return Digit8;
    case Scancode::Digit9:
      return Digit9;
    case Scancode::Digit0:
      return Digit0;
    case Scancode::Minus:
      return Minus;
    case Scancode::Equals:
      return Equals;
    case Scancode::Backspace:
      return Backspace;
    case Scancode::Tab:
      return Tab;
    case Scancode::Q:
      return Q;
    case Scancode::W:
      return W;
    case Scancode::E:
      return E;
    case Scancode::R:
      return R;
    case Scancode::T:
      return T;
    case Scancode::Y:
      return Y;
    case Scancode::U:
      return U;
    case Scancode::I:
      return I;
    case Scancode::O:
      return O;
    case Scancode::P:
      return P;
    case Scancode::BracketLeft:
      return BracketLeft;
    case Scancode::BracketRight:
      return BracketRight;
    case Scancode::Backslash:
      return Backslash;
    case Scancode::CapsLock:
      return CapsLock;
    case Scancode::A:
      return A;
    case Scancode::S:
      return S;
    case Scancode::D:
      return D;
    case Scancode::F:
      return F;
    case Scancode::G:
      return G;
    case Scancode::H:
      return H;
    case Scancode::J:
      return J;
    case Scancode::K:
      return K;
    case Scancode::L:
      return L;
    case Scancode::Semicolon:
      return Semicolon;
    case Scancode::Apostrophe:
      return Apostrophe;
    case Scancode::Enter:
      return Enter;
    case Scancode::ShiftLeft:
      return ShiftLeft;
    case Scancode::Z:
      return Z;
    case Scancode::X:
      return X;
    case Scancode::C:
      return C;
    case Scancode::V:
      return V;
    case Scancode::B:
      return B;
    case Scancode::N:
      return N;
    case Scancode::M:
      return M;
    case Scancode::Comma:
      return Comma;
    case Scancode::Period:
      return Period;
    case Scancode::Slash:
      return Slash;
    case Scancode::ShiftRight:
      return ShiftRight;
    case Scancode::ControlLeft:
      return ControlLeft;
    case Scancode::MetaLeft:
      return SuperLeft;
    case Scancode::AltLeft:
      return AltLeft;
    case Scancode::Space:
      return Space;
    case Scancode::AltRight:
      return AltRight;
    case Scancode::MetaRight:
      return SuperRight;
    case Scancode::Application:
      return Application;
    case Scancode::ControlRight:
      return ControlRight;
    default:
      return Unknown;
  }
}

AnsiKey VirtualKeyToKey(uint8_t virtual_key) {
  using enum AnsiKey;
  switch (virtual_key) {
    case VK_ESCAPE:
      return Escape;
    case VK_F1:
      return F1;
    case VK_F2:
      return F2;
    case VK_F3:
      return F3;
    case VK_F4:
      return F4;
    case VK_F5:
      return F5;
    case VK_F6:
      return F6;
    case VK_F7:
      return F7;
    case VK_F8:
      return F8;
    case VK_F9:
      return F9;
    case VK_F10:
      return F10;
    case VK_F11:
      return F11;
    case VK_F12:
      return F12;
    case VK_PRINT:  // fallthrough
    case VK_SNAPSHOT:
      return PrintScreen;
    case VK_SCROLL:
      return ScrollLock;
    case VK_PAUSE:
      return Pause;
    case VK_INSERT:
      return Insert;
    case VK_DELETE:
      return Delete;
    case VK_HOME:
      return Home;
    case VK_END:
      return End;
    case VK_PRIOR:
      return PageUp;
    case VK_NEXT:
      return PageDown;
    case VK_UP:
      return Up;
    case VK_DOWN:
      return Down;
    case VK_LEFT:
      return Left;
    case VK_RIGHT:
      return Right;
    case VK_NUMLOCK:
      return NumLock;
    case VK_DIVIDE:
      return NumpadDivide;
    case VK_MULTIPLY:
      return NumpadMultiply;
    case VK_SUBTRACT:
      return NumpadMinus;
    case VK_ADD:
      return NumpadPlus;
    // case VK_RETURN:
    //   return NumpadEnter;
    case VK_DECIMAL:
      return NumpadPeriod;
    case VK_NUMPAD0:
      return Numpad0;
    case VK_NUMPAD1:
      return Numpad1;
    case VK_NUMPAD2:
      return Numpad2;
    case VK_NUMPAD3:
      return Numpad3;
    case VK_NUMPAD4:
      return Numpad4;
    case VK_NUMPAD5:
      return Numpad5;
    case VK_NUMPAD6:
      return Numpad6;
    case VK_NUMPAD7:
      return Numpad7;
    case VK_NUMPAD8:
      return Numpad8;
    case VK_NUMPAD9:
      return Numpad9;
    case VK_OEM_3:
      return Grave;
    case '1':
      return Digit1;
    case '2':
      return Digit2;
    case '3':
      return Digit3;
    case '4':
      return Digit4;
    case '5':
      return Digit5;
    case '6':
      return Digit6;
    case '7':
      return Digit7;
    case '8':
      return Digit8;
    case '9':
      return Digit9;
    case '0':
      return Digit0;
    case VK_OEM_MINUS:
      return Minus;
    case VK_OEM_8:
      return Equals;
    case VK_BACK:
      return Backspace;
    case VK_TAB:
      return Tab;
    case 'Q':
      return Q;
    case 'W':
      return W;
    case 'E':
      return E;
    case 'R':
      return R;
    case 'T':
      return T;
    case 'Y':
      return Y;
    case 'U':
      return U;
    case 'I':
      return I;
    case 'O':
      return O;
    case 'P':
      return P;
    case VK_OEM_4:
      return BracketLeft;
    case VK_OEM_6:
      return BracketRight;
    case VK_OEM_5:
      return Backslash;
    case VK_CAPITAL:
      return CapsLock;
    case 'A':
      return A;
    case 'S':
      return S;
    case 'D':
      return D;
    case 'F':
      return F;
    case 'G':
      return G;
    case 'H':
      return H;
    case 'J':
      return J;
    case 'K':
      return K;
    case 'L':
      return L;
    case VK_OEM_1:
      return Semicolon;
    case VK_OEM_7:
      return Apostrophe;
    case VK_RETURN:
      return Enter;
    case VK_SHIFT:  // fallthrough
    case VK_LSHIFT:
      return ShiftLeft;
    case 'Z':
      return Z;
    case 'X':
      return X;
    case 'C':
      return C;
    case 'V':
      return V;
    case 'B':
      return B;
    case 'N':
      return N;
    case 'M':
      return M;
    case VK_OEM_COMMA:
      return Comma;
    case VK_OEM_PERIOD:
      return Period;
    case VK_OEM_2:
      return Slash;
    case VK_RSHIFT:
      return ShiftRight;
    case VK_CONTROL:  // fallthrough
    case VK_LCONTROL:
      return ControlLeft;
    case VK_LWIN:
      return SuperLeft;
    case VK_MENU:  // fallthrough
    case VK_LMENU:
      return AltLeft;
    case VK_SPACE:
      return Space;
    case VK_RMENU:
      return AltRight;
    case VK_RWIN:
      return SuperRight;
    case VK_APPS:
      return Application;
    case VK_RCONTROL:
      return ControlRight;
    case 0x0:
    case 0xff:
      return Unknown;
    default:
      LOG << "Unknown virtual key: 0x" << f("{:x}", virtual_key) << " (" << virtual_key << ")";
  }
  return Unknown;
}

uint32_t KeyToScanCode(automat::ui::AnsiKey key) {
  using enum AnsiKey;
  switch (key) {
    case Unknown:
      return (uint32_t)Scancode::Unknown;
    case Escape:
      return (uint32_t)Scancode::Escape;
    case F1:
      return (uint32_t)Scancode::F1;
    case F2:
      return (uint32_t)Scancode::F2;
    case F3:
      return (uint32_t)Scancode::F3;
    case F4:
      return (uint32_t)Scancode::F4;
    case F5:
      return (uint32_t)Scancode::F5;
    case F6:
      return (uint32_t)Scancode::F6;
    case F7:
      return (uint32_t)Scancode::F7;
    case F8:
      return (uint32_t)Scancode::F8;
    case F9:
      return (uint32_t)Scancode::F9;
    case F10:
      return (uint32_t)Scancode::F10;
    case F11:
      return (uint32_t)Scancode::F11;
    case F12:
      return (uint32_t)Scancode::F12;
    case PrintScreen:
      return (uint32_t)Scancode::PrintScreen;
    case ScrollLock:
      return (uint32_t)Scancode::ScrollLock;
    case Pause:
      return (uint32_t)Scancode::Pause;
    case Insert:
      return (uint32_t)Scancode::Insert;
    case Delete:
      return (uint32_t)Scancode::Delete;
    case Home:
      return (uint32_t)Scancode::Home;
    case End:
      return (uint32_t)Scancode::End;
    case PageUp:
      return (uint32_t)Scancode::PageUp;
    case PageDown:
      return (uint32_t)Scancode::PageDown;
    case Up:
      return (uint32_t)Scancode::ArrowUp;
    case Down:
      return (uint32_t)Scancode::ArrowDown;
    case Left:
      return (uint32_t)Scancode::ArrowLeft;
    case Right:
      return (uint32_t)Scancode::ArrowRight;
    case NumLock:
      return (uint32_t)Scancode::NumLock;
    case NumpadDivide:
      return (uint32_t)Scancode::NumpadDivide;
    case NumpadMultiply:
      return (uint32_t)Scancode::NumpadMultiply;
    case NumpadMinus:
      return (uint32_t)Scancode::NumpadMinus;
    case NumpadPlus:
      return (uint32_t)Scancode::NumpadPlus;
    case NumpadEnter:
      return (uint32_t)Scancode::NumpadEnter;
    case NumpadPeriod:
      return (uint32_t)Scancode::NumpadPeriod;
    case Numpad0:
      return (uint32_t)Scancode::Numpad0;
    case Numpad1:
      return (uint32_t)Scancode::Numpad1;
    case Numpad2:
      return (uint32_t)Scancode::Numpad2;
    case Numpad3:
      return (uint32_t)Scancode::Numpad3;
    case Numpad4:
      return (uint32_t)Scancode::Numpad4;
    case Numpad5:
      return (uint32_t)Scancode::Numpad5;
    case Numpad6:
      return (uint32_t)Scancode::Numpad6;
    case Numpad7:
      return (uint32_t)Scancode::Numpad7;
    case Numpad8:
      return (uint32_t)Scancode::Numpad8;
    case Numpad9:
      return (uint32_t)Scancode::Numpad9;
    case Grave:
      return (uint32_t)Scancode::Grave;
    case Digit1:
      return (uint32_t)Scancode::Digit1;
    case Digit2:
      return (uint32_t)Scancode::Digit2;
    case Digit3:
      return (uint32_t)Scancode::Digit3;
    case Digit4:
      return (uint32_t)Scancode::Digit4;
    case Digit5:
      return (uint32_t)Scancode::Digit5;
    case Digit6:
      return (uint32_t)Scancode::Digit6;
    case Digit7:
      return (uint32_t)Scancode::Digit7;
    case Digit8:
      return (uint32_t)Scancode::Digit8;
    case Digit9:
      return (uint32_t)Scancode::Digit9;
    case Digit0:
      return (uint32_t)Scancode::Digit0;
    case Minus:
      return (uint32_t)Scancode::Minus;
    case Equals:
      return (uint32_t)Scancode::Equals;
    case Backspace:
      return (uint32_t)Scancode::Backspace;
    case Tab:
      return (uint32_t)Scancode::Tab;
    case Q:
      return (uint32_t)Scancode::Q;
    case W:
      return (uint32_t)Scancode::W;
    case E:
      return (uint32_t)Scancode::E;
    case R:
      return (uint32_t)Scancode::R;
    case T:
      return (uint32_t)Scancode::T;
    case Y:
      return (uint32_t)Scancode::Y;
    case U:
      return (uint32_t)Scancode::U;
    case I:
      return (uint32_t)Scancode::I;
    case O:
      return (uint32_t)Scancode::O;
    case P:
      return (uint32_t)Scancode::P;
    case BracketLeft:
      return (uint32_t)Scancode::BracketLeft;
    case BracketRight:
      return (uint32_t)Scancode::BracketRight;
    case Backslash:
      return (uint32_t)Scancode::Backslash;
    case CapsLock:
      return (uint32_t)Scancode::CapsLock;
    case A:
      return (uint32_t)Scancode::A;
    case S:
      return (uint32_t)Scancode::S;
    case D:
      return (uint32_t)Scancode::D;
    case F:
      return (uint32_t)Scancode::F;
    case G:
      return (uint32_t)Scancode::G;
    case H:
      return (uint32_t)Scancode::H;
    case J:
      return (uint32_t)Scancode::J;
    case K:
      return (uint32_t)Scancode::K;
    case L:
      return (uint32_t)Scancode::L;
    case Semicolon:
      return (uint32_t)Scancode::Semicolon;
    case Apostrophe:
      return (uint32_t)Scancode::Apostrophe;
    case Enter:
      return (uint32_t)Scancode::Enter;
    case ShiftLeft:
      return (uint32_t)Scancode::ShiftLeft;
    case Z:
      return (uint32_t)Scancode::Z;
    case X:
      return (uint32_t)Scancode::X;
    case C:
      return (uint32_t)Scancode::C;
    case V:
      return (uint32_t)Scancode::V;
    case B:
      return (uint32_t)Scancode::B;
    case N:
      return (uint32_t)Scancode::N;
    case M:
      return (uint32_t)Scancode::M;
    case Comma:
      return (uint32_t)Scancode::Comma;
    case Period:
      return (uint32_t)Scancode::Period;
    case Slash:
      return (uint32_t)Scancode::Slash;
    case ShiftRight:
      return (uint32_t)Scancode::ShiftRight;
    case ControlLeft:
      return (uint32_t)Scancode::ControlLeft;
    case SuperLeft:
      return (uint32_t)Scancode::MetaLeft;
    case AltLeft:
      return (uint32_t)Scancode::AltLeft;
    case Space:
      return (uint32_t)Scancode::Space;
    case AltRight:
      return (uint32_t)Scancode::AltRight;
    case SuperRight:
      return (uint32_t)Scancode::MetaRight;
    case Application:
      return (uint32_t)Scancode::Application;
    case ControlRight:
      return (uint32_t)Scancode::ControlRight;
    default:
      return (uint32_t)Scancode::Unknown;
  }
}

uint8_t KeyToVirtualKey(automat::ui::AnsiKey key) {
  uint32_t scan_code = KeyToScanCode(key);
  HKL layout = GetKeyboardLayout(0);
  return MapVirtualKeyExA(scan_code, MAPVK_VSC_TO_VK_EX, layout);
}