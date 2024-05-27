#pragma once

#include <include/core/SkCanvas.h>
#include <include/core/SkPath.h>

#include <bitset>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include "animation.hh"
#include "fn.hh"
#include "math.hh"
#include "status.hh"
#include "str.hh"
#include "time.hh"

namespace automat::gui {

struct CaretOwner;
struct Keyboard;
struct Window;
struct Widget;
struct DrawContext;
struct Pointer;
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

maf::StrView ToStr(AnsiKey) noexcept;

struct Key {
  bool ctrl;
  bool alt;
  bool shift;
  bool windows;
  AnsiKey physical;
  AnsiKey logical;
  std::string text;
};

struct Caret final {
  Keyboard& keyboard;
  CaretOwner* owner = nullptr;
  SkPath shape;
  Path widget_path;
  time::SystemPoint last_blink;
  Caret(Keyboard& keyboard);
  ~Caret() = default;
  void PlaceIBeam(Vec2 position);
  SkPath MakeRootShape(animation::Context&) const;

  // Called by the CaretOwner to release this Caret.
  void Release();
};

struct CaretOwner {
  std::vector<Caret*> carets;
  virtual ~CaretOwner();

  // Called by the Keyboard infrastructure to make the CaretOwner release all resources related to
  // the caret. This ends the key input coming from this Caret.
  virtual void ReleaseCaret(Caret&) = 0;
  virtual Widget* CaretWidget() = 0;

  virtual void KeyDown(Caret&, Key);
  virtual void KeyUp(Caret&, Key);
};

struct KeyboardGrab;

// Base class for objects that can grab keyboard input.
struct KeyboardGrabber {
  virtual ~KeyboardGrabber() = default;

  // Called by the Keyboard infrastructure to make the KeyboardGrabber release all resources related
  // to this grab.
  virtual void ReleaseGrab(KeyboardGrab&) = 0;

  // Called by the Keyboard infrastructure to get the widget that is grabbing the keyboard.
  virtual Widget* GrabWidget() = 0;

  virtual void KeyboardGrabberKeyDown(KeyboardGrab&, Key) {}
  virtual void KeyboardGrabberKeyUp(KeyboardGrab&, Key) {}
};

#if defined(_WIN32)

// This function should be called by the main windows thread, when a WM_HOTKEY message is received.
void OnHotKeyDown(int id);

#endif

// Represents a keyboard grab. Can be used to manipulate it.
struct KeyboardGrab final {
  Keyboard& keyboard;
  KeyboardGrabber& grabber;
  KeyboardGrab(Keyboard& keyboard, KeyboardGrabber& grabber)
      : keyboard(keyboard), grabber(grabber) {}

  // This will also call `ReleaseGrab` of its KeyboardGrabber.
  // This means that the pointers in the KeyboardGrabber will be invalid (nullptr) after this call!
  void Release();
};

struct KeyGrab;

struct KeyGrabber {
  virtual ~KeyGrabber() = default;

  // Called by the Keyboard infrastructure to make the KeyGrabber release all resources related to a
  // given KeyGrab
  virtual void ReleaseKeyGrab(KeyGrab&) = 0;

  virtual void KeyGrabberKeyDown(KeyGrab&) {}
  virtual void KeyGrabberKeyUp(KeyGrab&) {}
};

struct KeyGrab {
  Keyboard& keyboard;
  KeyGrabber& grabber;
  AnsiKey key;  // physical
  bool ctrl : 1;
  bool alt : 1;
  bool shift : 1;
  bool windows : 1;
#if defined(_WIN32)

  int id;  // value from the `RegisterHotKey` Win32 API

  // RegisterHotKey can only be called from the main windows thread.
  // This structure allows us to safely call it and return its status code.
  struct RegistrationCallback {
    KeyGrab* grab;                   // if null, the grab is cancelled, only used on Automat thread
    maf::Fn<void(maf::Status&)> fn;  // Windows thread schedules this on Automat thread
    maf::Status status;              // set on Windows thread, read on Automat thread
    RegistrationCallback(KeyGrab* grab, maf::Fn<void(maf::Status&)>&& fn) : grab(grab), fn(fn) {}
  };

  RegistrationCallback* cb = nullptr;  // only used on Automat thread
#endif
  KeyGrab(Keyboard& keyboard, KeyGrabber& grabber, AnsiKey key, bool ctrl, bool alt, bool shift,
          bool windows)
      : keyboard(keyboard),
        grabber(grabber),
        key(key),
        ctrl(ctrl),
        alt(alt),
        shift(shift),
        windows(windows) {}

  // This will also call `ReleaseGrab` of its KeyGrabber.
  // This means that the pointers in the KeyGrabber will be invalid (nullptr) after this call!
  void Release();
};

struct CaretAnimation {
  const Keyboard& keyboard;
  animation::DeltaFraction delta_fraction;
  SkPath shape;
  time::SystemPoint last_blink;
  animation::Approach fade_out;
  CaretAnimation(const Keyboard&);
};

struct KeyboardAnimation {
  std::map<Caret*, CaretAnimation> carets = {};
};

struct Keyboard final {
  Window& window;

  // Each keyboard may be associated with a pointer. This is the global OS pointer that may actually
  // aggregate multiple physical devices. If necessary, this should be switched to a collection of
  // pointers.
  Pointer* pointer = nullptr;

  // A keyboard can write to multiple carets at the same time! (not finished!)
  std::set<std::unique_ptr<Caret>> carets;
  std::bitset<static_cast<size_t>(AnsiKey::Count)> pressed_keys;
  mutable product_ptr<KeyboardAnimation> anim;

  std::unique_ptr<KeyboardGrab> grab;

  std::vector<std::unique_ptr<KeyGrab>> key_grabs;

  Keyboard(Window&);
  ~Keyboard();

  // Called by a CaretOwner that wants to start receiving keyboard input.
  Caret& RequestCaret(CaretOwner&, const Path& widget_path, Vec2 position);

  // Called by a KeyboardGrabber that wants to grab all keyboard events.
  KeyboardGrab& RequestGrab(KeyboardGrabber&);

  // Called by a KeyGrabber that wants to grab a key even when Automat is in the background.
  //
  // Callback is called with a Status object that contains the result of the grab request. It may be
  // called later, (after this function returns) depending on the OS load.
  KeyGrab& RequestKeyGrab(KeyGrabber&, AnsiKey key, bool ctrl, bool alt, bool shift, bool windows,
                          maf::Fn<void(maf::Status&)> cb);

  // Called by Automat drawing logic to draw the keyboard carets & other keyboard related visuals.
  void Draw(DrawContext&) const;

  // Called by the OS event loop to notify the Keyboard of a key press.
  void KeyDown(Key);

  // Called by the OS event loop to notify the Keyboard of a key release.
  void KeyUp(Key);
};

extern std::unique_ptr<gui::Keyboard> keyboard;

}  // namespace automat::gui
