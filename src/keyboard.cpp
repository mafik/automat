// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "keyboard.hpp"

#include <include/core/SkPathBuilder.h>
#include <xkbcommon/xkbcommon.h>

#include <algorithm>
#include <map>
#include <set>
#include <utility>

#include "animation.hpp"
#include "automat.hpp"
#include "font.hpp"
#include "keymap.hpp"
#include "root_widget.hpp"
#include "x11_keys.hpp"
#include "x11_xkb.hpp"

#if defined(_WIN32)
#include <Windows.h>

#include "win32.hpp"
#include "win32_window.hpp"
#include "win_key.hpp"

#endif

#if defined(__linux__)
#include <xcb/xinput.h>
#include <xcb/xproto.h>
#include <xcb/xtest.h>

#include "format.hpp"
#include "xcb.hpp"
#include "xcb_window.hpp"

#endif

namespace automat::ui {

std::unique_ptr<KeyboardGrab> Keyboard::grab;
Colony<KeyGrab> Keyboard::key_grabs;
Colony<Keylogging> Keyboard::keyloggings;
bool Keyboard::keyloggings_locked = false;

constexpr static AnsiKey kModifierKeys[] = {
    AnsiKey::ShiftLeft, AnsiKey::ShiftRight, AnsiKey::ControlLeft, AnsiKey::ControlRight,
    AnsiKey::AltLeft,   AnsiKey::AltRight,   AnsiKey::SuperLeft,   AnsiKey::SuperRight};

static Window* MainWindowOrNull() {
  if (!root_widget) return nullptr;
  return root_widget->window.get();
}

static bool Matches(const WeakPtr<Keyboard>& filter, const Keyboard& keyboard) {
  if (!filter) return true;
  return filter == &keyboard;
}

static SkPath PointerIBeam(const KeyboardWidget& keyboard) {
  if (keyboard.pointer) {
    float px = 1 / keyboard.root_widget.PxPerMeter();
    Vec2 pos = keyboard.pointer->PositionOnCanvas();
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
  if (!owner) return shape;
  return shape.makeTransform(TransformBetween(*owner, keyboard));
}

// Called by objects that want to grab all keyboard events in the system.
KeyboardGrab& Keyboard::RequestGrab(KeyboardGrabber& grabber, WeakPtr<Keyboard> keyboard) {
  if (grab) {
    grab->Release();
  }
  grab.reset(new KeyboardGrab(std::move(keyboard), grabber));
#ifdef __linux__
  // TODO: test whether this works
  auto& xcb_window = static_cast<xcb::XCBWindow&>(*root_widget->window);
  uint32_t mask = XCB_INPUT_XI_EVENT_MASK_KEY_PRESS | XCB_INPUT_XI_EVENT_MASK_KEY_RELEASE;
  auto cookie = xcb_input_xi_grab_device(xcb::connection, xcb::screen->root, XCB_CURRENT_TIME,
                                         XCB_CURSOR_NONE, xcb_window.master_keyboard_device_id,
                                         XCB_INPUT_GRAB_MODE_22_ASYNC, XCB_INPUT_GRAB_MODE_22_ASYNC,
                                         false, 1, &mask);
  std::unique_ptr<xcb_generic_error_t, DeleteWithFree> error;
  std::unique_ptr<xcb_input_xi_grab_device_reply_t, DeleteWithFree> reply(
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
                                  bool shift, bool windows, Fn<void(Status&)> cb,
                                  WeakPtr<Keyboard> keyboard) {
  KeyGrab& key_grab =
      *key_grabs.emplace(std::move(keyboard), key_grabber, key, ctrl, alt, shift, windows);
#if defined(_WIN32)
  // See https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-registerhotkey
  static int id_counter = 0;
  id_counter = (id_counter + 1) % 0xC000;
  key_grab.id = id_counter;
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
  key_grab.cb = new KeyGrab::RegistrationCallback(&key_grab, std::move(cb));

  auto& win32_window = dynamic_cast<Win32Window&>(*root_widget->window);
  win32_window.PostToMainLoop(
      [id = key_grab.id, modifiers, vk, cb = key_grab.cb, hwnd = win32_window.hwnd]() {
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
  return key_grab;
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
    anim.shape = anim.shape.makeOffset(0, timer.d * kLetterSize);
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

CaretAnimation::CaretAnimation(const KeyboardWidget& keyboard)
    : keyboard(keyboard), shape(PointerIBeam(keyboard)), last_blink(time::SteadyNow()) {}

ui::Tock KeyboardWidget::Tick(time::Timer& timer) {
  // Iterate through each Caret & CaretAnimation, and update their animations.
  // Animations may result in a Caret being removed.
  // After a Caret has been removed, its CaretAnimation is kept around for some
  // time to animate its disappearance.
  // Drop carets whose owner widget has been destroyed — leaves their anim entry behind so the
  // disappear animation toward the pointer ibeam plays.
  std::erase_if(carets, [](const std::unique_ptr<Caret>& c) { return !c->owner; });
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
    return Tock::Draw;
  } else {
    return Tock::Drawing;
  }
}

void KeyboardWidget::Draw(SkCanvas& canvas) const {
  SkPaint paint;
  paint.setColor(SK_ColorBLACK);
  paint.setAntiAlias(true);
  for (auto& [caret, anim] : anim.carets) {
    paint.setAlphaf(anim.alpha);
    canvas.drawPath(anim.shape, paint);
  }
}

SkPath KeyboardWidget::Shape() const {
  SkPathBuilder builder;
  for (auto& caret : carets) {
    builder.addPath(caret->MakeRootShape());
  }
  return builder.detach();
}

static AnsiKey KeysymToKey(uint32_t keysym) {
  using enum AnsiKey;
  if (keysym >= 'a' && keysym <= 'z') keysym += 'A' - 'a';
  switch (keysym) {
    case 'A':
      return A;
    case 'B':
      return B;
    case 'C':
      return C;
    case 'D':
      return D;
    case 'E':
      return E;
    case 'F':
      return F;
    case 'G':
      return G;
    case 'H':
      return H;
    case 'I':
      return I;
    case 'J':
      return J;
    case 'K':
      return K;
    case 'L':
      return L;
    case 'M':
      return M;
    case 'N':
      return N;
    case 'O':
      return O;
    case 'P':
      return P;
    case 'Q':
      return Q;
    case 'R':
      return R;
    case 'S':
      return S;
    case 'T':
      return T;
    case 'U':
      return U;
    case 'V':
      return V;
    case 'W':
      return W;
    case 'X':
      return X;
    case 'Y':
      return Y;
    case 'Z':
      return Z;
    case '0':
      return Digit0;
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
    case '`':
      return Grave;
    case '-':
      return Minus;
    case '=':
      return Equals;
    case '[':
      return BracketLeft;
    case ']':
      return BracketRight;
    case '\\':
      return Backslash;
    case ';':
      return Semicolon;
    case '\'':
      return Apostrophe;
    case ',':
      return Comma;
    case '.':
      return Period;
    case '/':
      return Slash;
    case ' ':
      return Space;
  }
  return Unknown;
}

void Keyboard::Translate(Key& key, bool down, U32 mods, U32 group) {
  key.logical = key.physical;
  if (!keymap) return;
  Keymap::xkb_state_ptr state{xkb_state_new(keymap->xkb.get())};
  if (!state) return;
  xkb_state_update_mask(state.get(), mods, 0, 0, group, 0, 0);

  U32 effective = xkb_state_serialize_mods(state.get(), XKB_STATE_MODS_EFFECTIVE);
  key.shift = effective & 0x01;
  key.caps_lock = effective & 0x02;
  key.ctrl = effective & 0x04;
  key.alt = effective & 0x08;
  key.num_lock = effective & 0x10;
  key.level5 = effective & 0x20;
  key.windows = effective & 0x40;
  key.alt_gr = effective & 0x80;
  key.layout = (U8)xkb_state_serialize_layout(state.get(), XKB_STATE_LAYOUT_EFFECTIVE);

  U32 keycode = (U32)x11::KeyToX11KeyCode(key.physical);
  if (keycode <= 8) return;
  xkb_layout_index_t key_layout = xkb_state_key_get_layout(state.get(), keycode);
  const xkb_keysym_t* syms = nullptr;
  if (xkb_keymap_key_get_syms_by_level(keymap->xkb.get(), keycode, key_layout, 0, &syms) > 0) {
    AnsiKey logical = KeysymToKey(syms[0]);
    if (logical != AnsiKey::Unknown) key.logical = logical;
  }
  if (down) {
    char buffer[32];
    int size = xkb_state_key_get_utf8(state.get(), keycode, buffer, sizeof(buffer));
    key.text.assign(buffer, std::clamp<int>(size, 0, sizeof(buffer) - 1));
  }
}

bool Keyboard::TranslateRawKey(Key& key, bool down, int group) {
  if (key.physical > AnsiKey::Unknown && key.physical < AnsiKey::Count) {
    if (pressed_keys[(size_t)key.physical] == down) return false;
    pressed_keys[(size_t)key.physical] = down;
  }
  if (down && key.physical == AnsiKey::CapsLock) caps_lock_on = !caps_lock_on;
  if (down && key.physical == AnsiKey::NumLock) num_lock_on = !num_lock_on;
  U32 mods = 0;
  if (keymap) {
    for (AnsiKey modifier : kModifierKeys) {
      if (pressed_keys[(size_t)modifier]) {
        mods |= keymap->key_mods[(U8)x11::KeyToX11KeyCode(modifier)];
      }
    }
    if (caps_lock_on) mods |= keymap->key_mods[(size_t)x11::KeyCode::CapsLock];
    if (num_lock_on) mods |= keymap->key_mods[(size_t)x11::KeyCode::NumLock];
  }
  Translate(key, down, mods, (U32)std::max(group, 0));
  return true;
}

#ifdef __linux__

void KeyboardWidget::KeyDown(xcb_input_key_press_event_t& ev) {
  if (ev.flags & XCB_INPUT_KEY_EVENT_FLAGS_KEY_REPEAT) return;
  ui::Key key = {.physical = x11::X11KeyCodeToKey((x11::KeyCode)ev.detail)};
  keyboard->Translate(key, true, ev.mods.effective, ev.group.effective);
  KeyDown(key);
}
void KeyboardWidget::KeyDown(xcb_input_raw_key_press_event_t& ev) {
  if (ev.flags & XCB_INPUT_KEY_EVENT_FLAGS_KEY_REPEAT) return;
  ui::Key key = {.physical = x11::X11KeyCodeToKey((x11::KeyCode)ev.detail),
                 .logical = x11::X11KeyCodeToKey((x11::KeyCode)ev.detail)};
  LogKeyDown(key);
}
void KeyboardWidget::KeyDown(xcb_key_press_event_t& ev) {
  ui::Key key = {
      .physical = x11::X11KeyCodeToKey((x11::KeyCode)ev.detail),
      .logical = x11::X11KeyCodeToKey((x11::KeyCode)ev.detail),
  };
  KeyDown(key);
}

void KeyboardWidget::KeyUp(xcb_input_key_release_event_t& ev) {
  ui::Key key = {.physical = x11::X11KeyCodeToKey((x11::KeyCode)ev.detail)};
  keyboard->Translate(key, false, ev.mods.effective, ev.group.effective);
  KeyUp(key);
}
void KeyboardWidget::KeyUp(xcb_input_raw_key_release_event_t& ev) {
  ui::Key key = {.physical = x11::X11KeyCodeToKey((x11::KeyCode)ev.detail),
                 .logical = x11::X11KeyCodeToKey((x11::KeyCode)ev.detail)};
  LogKeyUp(key);
}
void KeyboardWidget::KeyUp(xcb_key_release_event_t& ev) {
  ui::Key key = {.physical = x11::X11KeyCodeToKey((x11::KeyCode)ev.detail),
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

void KeyboardWidget::KeyDown(Key key) {
  // Quit on Ctrl + Q
  if (key.ctrl && key.physical == AnsiKey::Q) {
    stop_source.request_stop();
    return;
  }
  if (Keyboard::grab && Matches(Keyboard::grab->keyboard, *keyboard)) {
    // KeyboardGrabber takes over all key events
    Keyboard::grab->grabber.KeyboardGrabberKeyDown(*Keyboard::grab, key);
  } else if (key.physical == AnsiKey::Escape) {
    // Release the carets when Escape is pressed
    DeleteSafeForEach(carets, [](Caret& caret) {
      if (caret.owner) caret.owner->ReleaseCaret(caret);
    });
    carets.clear();
  } else if (!carets.empty()) {
    // The list of carets may be modified by the KeyDown. Because of that we have to iterate over
    // the list of carets in a very careful way.
    DeleteSafeForEach(carets, [key](Caret& caret) {
      if (caret.owner) caret.owner->KeyDown(caret, key);
    });
  } else {
    size_t i = static_cast<int>(key.physical);
    if (actions[i] == nullptr && pointer && pointer->hover) {
      Widget* current = pointer->hover;
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

void KeyboardWidget::KeyUp(Key key) {
  if (Keyboard::grab && Matches(Keyboard::grab->keyboard, *keyboard)) {
    Keyboard::grab->grabber.KeyboardGrabberKeyUp(*Keyboard::grab, key);
  } else if (!carets.empty()) {
    DeleteSafeForEach(carets, [key](Caret& caret) {
      if (caret.owner) caret.owner->KeyUp(caret, key);
    });
  } else {
    size_t i = static_cast<int>(key.physical);
    if (actions[i]) {
      actions[i].reset();
      pointer->UpdatePath();
    }
  }
}

static void KeyloggingsGC() {
  if (Keyboard::keyloggings.empty()) {
    return;
  }
  // Remove all released keyloggings.
  for (auto it = Keyboard::keyloggings.begin(); it != Keyboard::keyloggings.end();) {
    if (it->released) {
      it = Keyboard::keyloggings.erase(it);
    } else {
      ++it;
    }
  }
  if (Keyboard::keyloggings.empty()) {
    if (Window* window = MainWindowOrNull()) window->RegisterInput();
  }
}

void KeyboardWidget::LogKeyDown(Key key) {
  Keyboard::keyloggings_locked = true;
  for (auto& keylogging : Keyboard::keyloggings) {
    if (!Matches(keylogging.keyboard, *keyboard)) continue;
    keylogging.keylogger.KeyloggerKeyDown(key);
  }
  Keyboard::keyloggings_locked = false;
  KeyloggingsGC();
}

void KeyboardWidget::LogKeyUp(Key key) {
  Keyboard::keyloggings_locked = true;
  for (auto& keylogging : Keyboard::keyloggings) {
    if (!Matches(keylogging.keyboard, *keyboard)) continue;
    keylogging.keylogger.KeyloggerKeyUp(key);
  }
  Keyboard::keyloggings_locked = false;
  KeyloggingsGC();
}

void Keyboard::SendKeyEvent(AnsiKey physical, bool down) {
#if defined(_WIN32)
  INPUT input = {};
  input.type = INPUT_KEYBOARD;
  input.ki.wScan = KeyToScanCode(physical);
  input.ki.dwFlags = KEYEVENTF_SCANCODE;
  if (!down) {
    input.ki.dwFlags |= KEYEVENTF_KEYUP;
  }
  if (input.ki.wScan & 0xff00) {
    input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    input.ki.wScan &= 0x00ff;  // not sure if it's necessary
  }
  SendInput(1, &input, sizeof(INPUT));
#endif
#if defined(__linux__)
  xcb_test_fake_input(xcb::connection, down ? XCB_KEY_PRESS : XCB_KEY_RELEASE,
                      (uint8_t)x11::KeyToX11KeyCode(physical), XCB_CURRENT_TIME, xcb::screen->root,
                      0, 0, 0);
  xcb_flush(xcb::connection);
#endif
}

Caret::Caret(KeyboardWidget& keyboard) : keyboard(keyboard) {}

Caret& KeyboardWidget::RequestCaret(Widget& caret_owner, Vec2 position) {
  std::set<std::unique_ptr<Caret>>::iterator it;
  if (carets.empty()) {
    it = carets.emplace(std::make_unique<Caret>(*this)).first;
  } else {
    it = carets.begin();
  }
  Caret& caret = **it;
  if (caret.owner) {
    caret.owner->ReleaseCaret(caret);
  }
  caret.owner = &caret_owner;
  caret.PlaceIBeam(position);
  WakeAnimation();
  return caret;
}

KeyboardWidget::KeyboardWidget(RootWidget& root_widget)
    : Widget(&root_widget), root_widget(root_widget), keyboard(new Keyboard()) {
  root_widget.layers.OrderInside(this);
}

KeyboardWidget::~KeyboardWidget() {
  while (!carets.empty()) {
    (*carets.begin())->Release();
  }
}

#if defined(_WIN32)

void OnHotKeyDown(int id) {
  bool handled = false;
  for (auto& key_grab : Keyboard::key_grabs) {
    if (key_grab.id == id) {
      key_grab.grabber.KeyGrabberKeyDown(key_grab);
      key_grab.grabber.KeyGrabberKeyUp(key_grab);
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
  auto& xcb_window = static_cast<xcb::XCBWindow&>(*root_widget->window);
  xcb_void_cookie_t cookie = xcb_input_xi_ungrab_device(xcb::connection, XCB_CURRENT_TIME,
                                                        xcb_window.master_keyboard_device_id);
  if (std::unique_ptr<xcb_generic_error_t, DeleteWithFree> error{
          xcb_request_check(xcb::connection, cookie)}) {
    ERROR << "Failed to ungrab the keyboard";
  }
#endif
  grabber.ReleaseGrab(*this);
  Keyboard::grab.reset();  // KeyboardGrab deletes itself here!
}

void KeyGrab::Release() {
#if defined(_WIN32)
  if (cb) {
    cb->grab = nullptr;
    cb = nullptr;
  }
  auto& win32_window = dynamic_cast<Win32Window&>(*root_widget->window);
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
  Keyboard::key_grabs.erase(Keyboard::key_grabs.get_iterator(this));  // KeyGrab deletes itself here!
}

void Keylogging::Release() {
  released = true;
  keylogger.KeyloggerOnRelease(*this);
  if (Keyboard::keyloggings_locked) {
    return;
  }
  Keyboard::keyloggings.erase(Keyboard::keyloggings.get_iterator(this));  // `this` is deleted here!
  if (Window* window = MainWindowOrNull()) window->RegisterInput();
}

}  // namespace automat::ui
