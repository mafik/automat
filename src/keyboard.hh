// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <include/core/SkCanvas.h>
#include <include/core/SkPath.h>

#include <bitset>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include "action.hh"
#include "animation.hh"
#include "fn.hh"
#include "math.hh"
#include "status.hh"
#include "time.hh"
#include "widget.hh"

#if defined(__linux__)
#include <xcb/xinput.h>
#endif  // defined(__linux__)

namespace automat::gui {

struct CaretOwner;
struct Keyboard;
struct RootWidget;
struct Widget;
struct Pointer;

void SendKeyEvent(AnsiKey physical, bool down);

struct Caret final {
  Keyboard& keyboard;
  CaretOwner* owner = nullptr;
  SkPath shape;
  std::shared_ptr<Widget> widget;
  Caret(Keyboard& keyboard);
  ~Caret() = default;
  void PlaceIBeam(Vec2 position);
  SkPath MakeRootShape() const;

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

  virtual void KeyboardGrabberKeyDown(KeyboardGrab&, Key) {}
  virtual void KeyboardGrabberKeyUp(KeyboardGrab&, Key) {}
};

struct Keylogger {
  virtual ~Keylogger() = default;
  virtual void KeyloggerKeyDown(Key) {}
  virtual void KeyloggerKeyUp(Key) {}
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

struct Keylogging {
  Keyboard& keyboard;
  Keylogger& keylogger;
  Keylogging(Keyboard& keyboard, Keylogger& keylogger) : keyboard(keyboard), keylogger(keylogger) {}
  void Release();
};

struct CaretAnimation {
  const Keyboard& keyboard;
  SkPath shape;
  time::SteadyPoint last_blink;
  float alpha = 1;
  CaretAnimation(const Keyboard&);
};

struct KeyboardAnimation {
  std::map<Caret*, CaretAnimation> carets = {};
};

struct Keyboard final : Widget {
  RootWidget& root_widget;

  // Each keyboard may be associated with a pointer. This is the global OS pointer that may actually
  // aggregate multiple physical devices. If necessary, this should be switched to a collection of
  // pointers.
  Pointer* pointer = nullptr;

  // A keyboard can write to multiple carets at the same time! (not finished!)
  std::set<std::unique_ptr<Caret>> carets;
  std::bitset<static_cast<size_t>(AnsiKey::Count)> pressed_keys;
  mutable KeyboardAnimation anim;

  std::unique_ptr<KeyboardGrab> grab;

  std::vector<std::unique_ptr<KeyGrab>> key_grabs;
  std::vector<std::unique_ptr<Keylogging>> keyloggings;
  std::unique_ptr<Action> actions[static_cast<size_t>(AnsiKey::Count)];

  Keyboard(RootWidget&);

  // Called by a CaretOwner that wants to start receiving keyboard input.
  Caret& RequestCaret(CaretOwner&, const std::shared_ptr<Widget>& widget, Vec2 position);

  // Called by a KeyboardGrabber that wants to grab all keyboard events.
  KeyboardGrab& RequestGrab(KeyboardGrabber&);

  // Called by a KeyGrabber that wants to grab a key even when Automat is in the background.
  //
  // Callback is called with a Status object that contains the result of the grab request. It may be
  // called later, (after this function returns) depending on the OS load.
  KeyGrab& RequestKeyGrab(KeyGrabber&, AnsiKey key, bool ctrl, bool alt, bool shift, bool windows,
                          maf::Fn<void(maf::Status&)> cb);

  Keylogging& BeginKeylogging(Keylogger&);

  animation::Phase Tick(time::Timer&) override;
  void Draw(SkCanvas&) const override;
  SkPath Shape() const override;
  maf::Optional<Rect> TextureBounds() const override { return std::nullopt; }

#if defined(__linux__)
  // TODO: refactor this
  void KeyDown(xcb_input_key_press_event_t&);
  void KeyDown(xcb_input_raw_key_press_event_t&);
  void KeyDown(xcb_key_press_event_t&);
  void KeyUp(xcb_input_key_release_event_t&);
  void KeyUp(xcb_input_raw_key_release_event_t&);
  void KeyUp(xcb_key_press_event_t&);
#endif  // defined(__linux__)

  // Called by the OS event loop to notify the Keyboard of a key press.
  void KeyDown(Key);

  // Called by the OS event loop to notify the Keyboard of a key release.
  void KeyUp(Key);

  void LogKeyDown(Key);

  void LogKeyUp(Key);
};

extern std::shared_ptr<gui::Keyboard> keyboard;

}  // namespace automat::gui
