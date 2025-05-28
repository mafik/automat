// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "keyboard.hh"

#include <include/core/SkPathBuilder.h>

#include <map>
#include <set>

#include "animation.hh"
#include "automat.hh"
#include "font.hh"
#include "root_widget.hh"

#if defined(_WIN32)
#include <Windows.h>

#include "win32.hh"
#include "win32_window.hh"
#include "win_key.hh"

#endif

#if defined(__linux__)
#include <xcb/xinput.h>
#include <xcb/xproto.h>
#include <xcb/xtest.h>
#include <xkbcommon/xkbcommon-x11.h>
#include <xkbcommon/xkbcommon.h>

#include "format.hh"
#include "x11.hh"
#include "xcb.hh"
#include "xcb_window.hh"

#endif

namespace automat::gui {

static SkPath PointerIBeam(const Keyboard& keyboard) {
  if (keyboard.pointer) {
    float px = 1 / keyboard.root_widget.PxPerMeter();
    Vec2 pos = keyboard.pointer->PositionWithinRootMachine();
    SkRect bounds = SkRect::MakeXYWH(pos.x, pos.y, 0, 0);
    switch (keyboard.pointer->Icon()) {
      case Pointer::IconType::kIconArrow:
        bounds.fRight += 2 * px;
        bounds.fTop -= 16 * px;
        break;
      case Pointer::IconType::kIconIBeam:
        bounds.fRight += px;
        bounds.fTop -= 9 * px;
        bounds.fBottom += 8 * px;
        break;
      default:
        bounds.fRight += 2 * px;
        bounds.fTop -= 2 * px;
        break;
    }
    return SkPath::Rect(bounds);
  } else {
    return SkPath();
  }
}

void Caret::PlaceIBeam(Vec2 position) {
  float width = GetFont().line_thickness;
  float height = kLetterSize;
  shape = SkPath::Rect(SkRect::MakeXYWH(position.x - width / 2, position.y, width, height));
}

void Caret::Release() {
  if (owner) {
    owner->ReleaseCaret(*this);
    if (auto it = std::find(owner->carets.begin(), owner->carets.end(), this);
        it != owner->carets.end()) {
      owner->carets.erase(it);
    }
    owner = nullptr;
  }
  for (auto it = keyboard.carets.begin(); it != keyboard.carets.end(); ++it) {
    if (it->get() == this) {
      keyboard.carets.erase(it);  // deletes this
      break;
    }
  }
}

SkPath Caret::MakeRootShape() const {
  SkMatrix text_to_root = TransformBetween(*widget, *root_machine);
  return shape.makeTransform(text_to_root);
}

// Called by objects that want to grab all keyboard events in the system.
KeyboardGrab& Keyboard::RequestGrab(KeyboardGrabber& grabber) {
  if (grab) {
    grab->Release();
  }
  grab.reset(new KeyboardGrab(*this, grabber));
#ifdef __linux__
  // TODO: test whether this works
  auto& xcb_window = static_cast<xcb::XCBWindow&>(*root_widget.window);
  uint32_t mask = XCB_INPUT_XI_EVENT_MASK_KEY_PRESS | XCB_INPUT_XI_EVENT_MASK_KEY_RELEASE;
  auto cookie = xcb_input_xi_grab_device(xcb::connection, xcb::screen->root, XCB_CURRENT_TIME,
                                         XCB_CURSOR_NONE, xcb_window.master_keyboard_device_id,
                                         XCB_INPUT_GRAB_MODE_22_ASYNC, XCB_INPUT_GRAB_MODE_22_ASYNC,
                                         false, 1, &mask);
  std::unique_ptr<xcb_generic_error_t, xcb::FreeDeleter> error;
  std::unique_ptr<xcb_input_xi_grab_device_reply_t, xcb::FreeDeleter> reply(
      xcb_input_xi_grab_device_reply(xcb::connection, cookie, std::out_ptr(error)));
  if (reply) {
    if (reply->status != XCB_GRAB_STATUS_SUCCESS) {
      ERROR << "Failed to grab the keyboard: " << reply->status;
    }
  }

  if (error) {
    ERROR << "Error while attempting to grab keyboard: " << dump_struct(*error);
  }
#endif
  return *grab;
}

KeyGrab& Keyboard::RequestKeyGrab(KeyGrabber& key_grabber, AnsiKey key, bool ctrl, bool alt,
                                  bool shift, bool windows, Fn<void(Status&)> cb) {
  auto key_grab = std::make_unique<KeyGrab>(*this, key_grabber, key, ctrl, alt, shift, windows);
#if defined(_WIN32)
  // See https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-registerhotkey
  static int id_counter = 0;
  id_counter = (id_counter + 1) % 0xC000;
  key_grab->id = id_counter;
  U32 modifiers = MOD_NOREPEAT;
  if (ctrl) {
    modifiers |= MOD_CONTROL;
  }
  if (alt) {
    modifiers |= MOD_ALT;
  }
  if (shift) {
    modifiers |= MOD_SHIFT;
  }
  if (windows) {
    modifiers |= MOD_WIN;
  }
  U8 vk = KeyToVirtualKey(key);
  key_grab->cb = new KeyGrab::RegistrationCallback(key_grab.get(), std::move(cb));

  auto& win32_window = dynamic_cast<Win32Window&>(*root_widget.window);
  win32_window.PostToMainLoop(
      [id = key_grab->id, modifiers, vk, cb = key_grab->cb, hwnd = win32_window.hwnd]() {
        bool success = RegisterHotKey(hwnd, id, modifiers, vk);
        if (!success) {
          AppendErrorMessage(cb->status) = "Failed to register hotkey: " + win32::GetLastErrorStr();
        }
        if (cb->grab) {
          cb->grab->cb = nullptr;
          cb->fn(cb->status);
        }
        delete cb;
      });
#else
  U16 modifiers = 0;
  if (ctrl) {
    modifiers |= XCB_MOD_MASK_CONTROL;
  }
  if (alt) {
    modifiers |= XCB_MOD_MASK_1;
  }
  if (shift) {
    modifiers |= XCB_MOD_MASK_SHIFT;
  }
  if (windows) {
    modifiers |= XCB_MOD_MASK_4;
  }
  xcb_keycode_t keycode = (U8)x11::KeyToX11KeyCode(key);

  for (bool caps_lock : {true, false}) {
    for (bool num_lock : {true, false}) {
      for (bool scroll_lock : {true, false}) {
        for (bool level3shift : {true, false}) {
          modifiers =
              caps_lock ? (modifiers | XCB_MOD_MASK_LOCK) : (modifiers & ~XCB_MOD_MASK_LOCK);
          modifiers = num_lock ? (modifiers | XCB_MOD_MASK_2) : (modifiers & ~XCB_MOD_MASK_2);
          modifiers = scroll_lock ? (modifiers | XCB_MOD_MASK_5) : (modifiers & ~XCB_MOD_MASK_5);
          modifiers = level3shift ? (modifiers | XCB_MOD_MASK_3) : (modifiers & ~XCB_MOD_MASK_3);
          auto cookie = xcb_grab_key(xcb::connection, 0, xcb::screen->root, modifiers, keycode,
                                     XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
          if (auto err = xcb_request_check(xcb::connection, cookie)) {
            FATAL << "Failed to grab key: " << err->error_code;
          }
        }
      }
    }
  }
#endif
  key_grabs.emplace_back(std::move(key_grab));
  return *key_grabs.back().get();
}

Keylogging& Keyboard::BeginKeylogging(Keylogger& keylogger) {
  if (keyloggings.empty()) {
#ifdef __linux__
    struct input_event_mask {
      xcb_input_event_mask_t header = {
          .deviceid = XCB_INPUT_DEVICE_ALL_MASTER,
          .mask_len = 1,
      };
      uint32_t mask =
          XCB_INPUT_XI_EVENT_MASK_RAW_KEY_PRESS | XCB_INPUT_XI_EVENT_MASK_RAW_KEY_RELEASE;
    } event_mask;

    xcb_void_cookie_t cookie = xcb_input_xi_select_events_checked(
        xcb::connection, xcb::screen->root, 1, &event_mask.header);

    if (std::unique_ptr<xcb_generic_error_t> error{xcb_request_check(xcb::connection, cookie)}) {
      ERROR << f("Couldn't select X11 events for keylogging: %d", error->error_code);
    }
#endif  // __linux__
#ifdef _WIN32
    auto& win32_window = dynamic_cast<Win32Window&>(*root_widget.window);
    win32_window.RegisterRawInput(true);
#endif
  }
  return *keyloggings.emplace_back(new Keylogging(*this, keylogger));
}

enum class CaretAnimAction { Keep, Delete };

static CaretAnimAction UpdateCaret(time::Timer& timer, CaretAnimation& anim, Caret* caret) {
  Optional<SkPath> target_path;
  float target_dist;
  bool disappear = false;
  if (caret) {
    disappear = false;
    target_path = caret->MakeRootShape();
  } else {
    disappear = true;
    // Animate disappearance of caret.
    if (anim.keyboard.pointer) {
      target_path = PointerIBeam(anim.keyboard);
    }
  }

  if (target_path.has_value()) {
    if (anim.shape.isInterpolatable(*target_path)) {
      SkPath out;
      float weight = 1;
      // The animation actually follows exponential curve.
      // TODO: Make this a warp curve instead.
      animation::LinearApproach(0, timer.d, 20, weight);
      anim.shape.interpolate(*target_path, weight, &out);
      anim.shape = out;
    } else {
      anim.shape = *target_path;
    }
    target_dist =
        SkPoint::Distance(target_path->getBounds().center(), anim.shape.getBounds().center());
    if (target_dist > 0.1_mm) {
      anim.alpha = 1;  // while animating caret movement, we always want the caret to be visible
    } else {
      // once at target, blink the caret on and off
      double seconds, subseconds;
      subseconds = modf(timer.NowSeconds(), &seconds);
      if (subseconds < 0.5) {
        anim.alpha = 1;
      } else {
        anim.alpha = 0;
      }
    }
  } else if (disappear) {
    animation::LinearApproach(0, timer.d, 1, anim.alpha);
    anim.shape.offset(0, timer.d * kLetterSize);
  }

  if (disappear) {
    if (target_path.has_value()) {
      if (target_dist < 0.1_mm) {
        return CaretAnimAction::Delete;
      }
    } else {
      if (anim.alpha < 0.01) {
        return CaretAnimAction::Delete;
      }
    }
  }

  return CaretAnimAction::Keep;
}

CaretAnimation::CaretAnimation(const Keyboard& keyboard)
    : keyboard(keyboard), shape(PointerIBeam(keyboard)), last_blink(time::SteadyNow()) {}

animation::Phase Keyboard::Tick(time::Timer& timer) {
  // Iterate through each Caret & CaretAnimation, and update their animations.
  // Animations may result in a Caret being removed.
  // After a Caret has been removed, its CaretAnimation is kept around for some
  // time to animate its disappearance.
  auto anim_it = anim.carets.begin();
  auto caret_it = carets.begin();
  while (anim_it != anim.carets.end() && caret_it != carets.end()) {
    if (anim_it->first < caret_it->get()) {
      // Caret was removed.
      auto a = UpdateCaret(timer, anim_it->second, nullptr);
      if (a == CaretAnimAction::Delete) {
        anim_it = anim.carets.erase(anim_it);
      } else {
        ++anim_it;
      }
    } else if (anim_it->first > caret_it->get()) {
      // Caret was added.
      auto new_it =
          anim.carets.emplace(std::make_pair<Caret*, CaretAnimation>(caret_it->get(), *this)).first;
      UpdateCaret(timer, new_it->second, caret_it->get());
      ++caret_it;
    } else {
      UpdateCaret(timer, anim_it->second, caret_it->get());
      ++anim_it;
      ++caret_it;
    }
  }
  while (anim_it != anim.carets.end()) {
    // Caret at end was removed.
    auto a = UpdateCaret(timer, anim_it->second, nullptr);
    if (a == CaretAnimAction::Delete) {
      anim_it = anim.carets.erase(anim_it);
    } else {
      ++anim_it;
    }
  }
  while (caret_it != carets.end()) {
    // Caret at end was added.
    auto new_it =
        anim.carets.emplace(std::make_pair<Caret*, CaretAnimation>(caret_it->get(), *this)).first;
    UpdateCaret(timer, new_it->second, caret_it->get());
    ++caret_it;
  }
  if (anim.carets.empty()) {
    return animation::Finished;
  } else {
    return animation::Animating;
  }
}

void Keyboard::Draw(SkCanvas& canvas) const {
  SkPaint paint;
  paint.setColor(SK_ColorBLACK);
  paint.setAntiAlias(true);
  for (auto& [caret, anim] : anim.carets) {
    paint.setAlphaf(anim.alpha);
    canvas.drawPath(anim.shape, paint);
  }
}

SkPath Keyboard::Shape() const {
  SkPathBuilder builder;
  for (auto& caret : carets) {
    builder.addPath(caret->MakeRootShape());
  }
  return builder.detach();
}

#ifdef __linux__

void Keyboard::KeyDown(xcb_input_key_press_event_t& ev) {
  gui::Key key = {
      .ctrl = static_cast<bool>(ev.mods.base & XCB_MOD_MASK_CONTROL),
      .alt = static_cast<bool>(ev.mods.base & XCB_MOD_MASK_1),
      .shift = static_cast<bool>(ev.mods.base & XCB_MOD_MASK_SHIFT),
      .windows = static_cast<bool>(ev.mods.base & XCB_MOD_MASK_4),
      .physical = x11::X11KeyCodeToKey((x11::KeyCode)ev.detail),
      .logical = x11::X11KeyCodeToKey((x11::KeyCode)ev.detail),
  };
  xkb_context* ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  int32_t device_id = ev.deviceid;
  xkb_keymap* keymap =
      xkb_x11_keymap_new_from_device(ctx, xcb::connection, device_id, XKB_KEYMAP_COMPILE_NO_FLAGS);
  xkb_state* state = xkb_x11_state_new_from_device(keymap, xcb::connection, device_id);

  char buffer[32];
  int size = xkb_state_key_get_utf8(state, ev.detail, buffer, sizeof(buffer));
  key.text.assign(buffer, size);

  xkb_keymap_unref(keymap);
  xkb_state_unref(state);
  xkb_context_unref(ctx);
  KeyDown(key);
}
void Keyboard::KeyDown(xcb_input_raw_key_press_event_t& ev) {
  gui::Key key = {
      .physical = x11::X11KeyCodeToKey((x11::KeyCode)ev.detail),
      .logical = x11::X11KeyCodeToKey((x11::KeyCode)ev.detail),
  };
  LogKeyDown(key);
}
void Keyboard::KeyDown(xcb_key_press_event_t& ev) {
  gui::Key key = {
      .physical = x11::X11KeyCodeToKey((x11::KeyCode)ev.detail),
      .logical = x11::X11KeyCodeToKey((x11::KeyCode)ev.detail),
  };
  KeyDown(key);
}

void Keyboard::KeyUp(xcb_input_key_release_event_t& ev) {
  gui::Key key = {.ctrl = static_cast<bool>(ev.mods.base & XCB_MOD_MASK_CONTROL),
                  .alt = static_cast<bool>(ev.mods.base & XCB_MOD_MASK_1),
                  .shift = static_cast<bool>(ev.mods.base & XCB_MOD_MASK_SHIFT),
                  .windows = static_cast<bool>(ev.mods.base & XCB_MOD_MASK_4),
                  .physical = x11::X11KeyCodeToKey((x11::KeyCode)ev.detail),
                  .logical = x11::X11KeyCodeToKey((x11::KeyCode)ev.detail)};
  KeyUp(key);
}
void Keyboard::KeyUp(xcb_input_raw_key_release_event_t& ev) {
  gui::Key key = {.physical = x11::X11KeyCodeToKey((x11::KeyCode)ev.detail),
                  .logical = x11::X11KeyCodeToKey((x11::KeyCode)ev.detail)};
  LogKeyUp(key);
}
void Keyboard::KeyUp(xcb_key_press_event_t& ev) {
  gui::Key key = {.physical = x11::X11KeyCodeToKey((x11::KeyCode)ev.detail),
                  .logical = x11::X11KeyCodeToKey((x11::KeyCode)ev.detail)};
  KeyUp(key);
}
#endif  // __linux__

// Helper for safely iterating over a list of carets. The list may be modified by the callback.
template <typename T>
void DeleteSafeForEach(std::set<std::unique_ptr<Caret>>& carets, const T& cb) {
  std::vector<Caret*> carets_copy;
  carets_copy.reserve(carets.size());
  for (auto& caret : carets) {
    carets_copy.push_back(caret.get());
  }
  // Then we iterate over this list of carets.
  for (auto* caret : carets_copy) {
    // For each caret we check if it's still in the list of carets.
    for (auto& c : carets) {
      if (c.get() == caret) {
        // Only if the caret is still present, we notify the CaretOwner.
        cb(*caret);
        break;
      }
    }
  }
}

void Keyboard::KeyDown(Key key) {
  // Quit on Ctrl + Q
  if (key.ctrl && key.physical == AnsiKey::Q) {
    Status status;
    automat::StopAutomat(status);
    if (!OK(status)) {
      ERROR << "Error while stopping Automat: " << status;
    }
    return;
  }
  if (key.physical > AnsiKey::Unknown && key.physical < AnsiKey::Count) {
    pressed_keys.set((size_t)key.physical);
  }
  if (grab) {
    // KeyboardGrabber takes over all key events
    grab->grabber.KeyboardGrabberKeyDown(*grab, key);
  } else if (key.physical == AnsiKey::Escape) {
    // Release the carets when Escape is pressed
    DeleteSafeForEach(carets, [](Caret& caret) { caret.owner->ReleaseCaret(caret); });
    carets.clear();
  } else if (!carets.empty()) {
    // The list of carets may be modified by the KeyDown. Because of that we have to iterate over
    // the list of carets in a very careful way.
    DeleteSafeForEach(carets, [key](Caret& caret) { caret.owner->KeyDown(caret, key); });
  } else {
    size_t i = static_cast<int>(key.physical);
    if (actions[i] == nullptr && pointer && pointer->hover) {
      auto current = pointer->hover;
      do {
        actions[i] = current->FindAction(*pointer, key.physical);
        current = current->parent;
      } while (actions[i] == nullptr && current);
      if (actions[i]) {
        pointer->UpdatePath();
      }
    }
  }
}

void Keyboard::KeyUp(Key key) {
  if (key.physical > AnsiKey::Unknown && key.physical < AnsiKey::Count) {
    pressed_keys.reset((size_t)key.physical);
  }
  if (grab) {
    grab->grabber.KeyboardGrabberKeyUp(*grab, key);
  } else if (!carets.empty()) {
    DeleteSafeForEach(carets, [key](Caret& caret) { caret.owner->KeyUp(caret, key); });
  } else {
    size_t i = static_cast<int>(key.physical);
    if (actions[i]) {
      actions[i].reset();
      pointer->UpdatePath();
    }
  }
}
void Keyboard::LogKeyDown(Key key) {
  for (auto& keylogging : keyloggings) {
    keylogging->keylogger.KeyloggerKeyDown(key);
  }
}

void Keyboard::LogKeyUp(Key key) {
  for (auto& keylogging : keyloggings) {
    keylogging->keylogger.KeyloggerKeyUp(key);
  }
}

Ptr<gui::Keyboard> keyboard;

void SendKeyEvent(AnsiKey physical, bool down) {
#if defined(_WIN32)
  INPUT input = {};
  input.type = INPUT_KEYBOARD;
  input.ki.wScan = KeyToScanCode(physical);
  input.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
  SendInput(1, &input, sizeof(INPUT));
#endif
#if defined(__linux__)
  xcb_test_fake_input(xcb::connection, down ? XCB_KEY_PRESS : XCB_KEY_RELEASE,
                      (uint8_t)x11::KeyToX11KeyCode(physical), XCB_CURRENT_TIME, xcb::screen->root,
                      0, 0, 0);
  xcb_flush(xcb::connection);
#endif
}

Caret::Caret(Keyboard& keyboard) : keyboard(keyboard) {}

CaretOwner::~CaretOwner() {
  for (auto caret : carets) {
    caret->Release();
  }
}

Caret& Keyboard::RequestCaret(CaretOwner& caret_owner, const Ptr<Widget>& widget, Vec2 position) {
  std::set<std::unique_ptr<Caret>>::iterator it;
  if (carets.empty()) {
    it = carets.emplace(std::make_unique<Caret>(*this)).first;
  } else {
    it = carets.begin();
  }
  Caret& caret = **it;
  if (caret.owner) {
    caret.owner->ReleaseCaret(caret);
    caret.owner->carets.erase(
        std::find(caret.owner->carets.begin(), caret.owner->carets.end(), &caret));
  }
  caret.owner = &caret_owner;
  caret.widget = widget;
  caret.PlaceIBeam(position);
  caret_owner.carets.emplace_back(&caret);
  WakeAnimation();
  return caret;
}

void CaretOwner::KeyDown(Caret& caret, Key) {}
void CaretOwner::KeyUp(Caret& caret, Key) {}

Keyboard::Keyboard(RootWidget& root_widget) : root_widget(root_widget) {}

#if defined(_WIN32)

void OnHotKeyDown(int id) {
  bool handled = false;
  for (auto& key_grab : keyboard->key_grabs) {
    if (key_grab->id == id) {
      key_grab->grabber.KeyGrabberKeyDown(*key_grab);
      key_grab->grabber.KeyGrabberKeyUp(*key_grab);
      handled = true;
      break;
    }
  }
  if (!handled) {
    ERROR << "Hotkey " << id << " not found";
  }
}

#endif

void KeyboardGrab::Release() {
#ifdef __linux__
  auto& xcb_window = static_cast<xcb::XCBWindow&>(*keyboard.root_widget.window);
  xcb_void_cookie_t cookie = xcb_input_xi_ungrab_device(xcb::connection, XCB_CURRENT_TIME,
                                                        xcb_window.master_keyboard_device_id);
  if (std::unique_ptr<xcb_generic_error_t, xcb::FreeDeleter> error{
          xcb_request_check(xcb::connection, cookie)}) {
    ERROR << "Failed to ungrab the keyboard";
  }
#endif
  grabber.ReleaseGrab(*this);
  keyboard.grab.reset();  // KeyboardGrab deletes itself here!
}

void KeyGrab::Release() {
#if defined(_WIN32)
  if (cb) {
    cb->grab = nullptr;
    cb = nullptr;
  }
  auto& win32_window = dynamic_cast<Win32Window&>(*keyboard.root_widget.window);
  win32_window.PostToMainLoop([id = id, hwnd = win32_window.hwnd]() {
    bool success = UnregisterHotKey(hwnd, id);
    if (!success) {
      ERROR << win32::GetLastErrorStr();
    }
  });
#else
  xcb_keycode_t keycode = (U8)x11::KeyToX11KeyCode(key);

  auto cookie =
      xcb_ungrab_key_checked(xcb::connection, keycode, xcb::screen->root, XCB_MOD_MASK_ANY);
  if (auto err = xcb_request_check(xcb::connection, cookie)) {
    FATAL << "Failed to ungrab key: " << err->error_code;
  }
#endif
  grabber.ReleaseKeyGrab(*this);
  for (auto it = keyboard.key_grabs.begin(); it != keyboard.key_grabs.end(); ++it) {
    if (it->get() == this) {
      keyboard.key_grabs.erase(it);  // KeyGrab deletes itself here!
      break;
    }
  }
}

void Keylogging::Release() {
  auto it = keyboard.keyloggings.begin();
  for (; it != keyboard.keyloggings.end(); ++it) {
    if (it->get() == this) {
      break;
    }
  }
  if (it == keyboard.keyloggings.end()) {
    return;
  }
  if (keyboard.keyloggings.size() == 1) {
#ifdef __linux__
    struct input_event_mask {
      xcb_input_event_mask_t header = {
          .deviceid = XCB_INPUT_DEVICE_ALL_MASTER,
          .mask_len = 1,
      };
      uint32_t mask = 0;
    } event_mask;

    xcb_void_cookie_t cookie = xcb_input_xi_select_events_checked(
        xcb::connection, xcb::screen->root, 1, &event_mask.header);

    if (std::unique_ptr<xcb_generic_error_t> error{xcb_request_check(xcb::connection, cookie)}) {
      ERROR << f("Couldn't release X11 event selection: %d", error->error_code);
    }
#endif  // __linux__
#ifdef _WIN32
    auto& win32_window = dynamic_cast<Win32Window&>(*keyboard.root_widget.window);
    win32_window.RegisterRawInput(false);
#endif
  }
  keyboard.keyloggings.erase(it);  // After this line `this` is deleted!
}

}  // namespace automat::gui
