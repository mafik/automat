#include "keyboard.hh"

#include <map>
#include <set>

#include "font.hh"
#include "window.hh"

#if defined(_WIN32)
#include <Windows.h>

#include "win.hh"
#include "win_key.hh"
#include "win_main.hh"
#endif

#if defined(__linux__)
#include <xcb/xinput.h>
#include <xcb/xproto.h>
#include <xcb/xtest.h>

#include "format.hh"
#include "linux_main.hh"
#include "x11.hh"

#endif

using namespace maf;

namespace automat::gui {

static SkPath PointerIBeam(const Keyboard& keyboard) {
  if (keyboard.pointer) {
    float px = 1 / keyboard.window.PxPerMeter();
    Vec2 pos = keyboard.pointer->PositionWithin(*root_machine);
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
  last_blink = time::SystemNow();
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
}

SkPath Caret::MakeRootShape(animation::Context& actx) const {
  auto begin = find(widget_path.begin(), widget_path.end(), root_machine);
  Path sub_path(begin, widget_path.end());
  SkMatrix text_to_root = TransformUp(sub_path, actx);
  return shape.makeTransform(text_to_root);
}

// Called by objects that want to grab all keyboard events in the system.
KeyboardGrab& Keyboard::RequestGrab(KeyboardGrabber& grabber) {
  if (grab) {
    grab->Release();
  }
  grab.reset(new KeyboardGrab(*this, grabber));
  return *grab;
}

KeyGrab& Keyboard::RequestKeyGrab(KeyGrabber& key_grabber, AnsiKey key, bool ctrl, bool alt,
                                  bool shift, bool windows, maf::Fn<void(maf::Status&)> cb) {
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
  RunOnWindowsThread([id = key_grab->id, modifiers, vk, cb = key_grab->cb]() {
    bool success = RegisterHotKey(main_window, id, modifiers, vk);
    if (!success) {
      AppendErrorMessage(cb->status) = "Failed to register hotkey: " + GetLastErrorStr();
    }
    RunOnAutomatThread([cb]() {
      if (cb->grab) {
        cb->grab->cb = nullptr;
        cb->fn(cb->status);
      }
      delete cb;
    });
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
          auto cookie = xcb_grab_key(connection, 0, screen->root, modifiers, keycode,
                                     XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
          if (auto err = xcb_request_check(connection, cookie)) {
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

    xcb_void_cookie_t cookie =
        xcb_input_xi_select_events_checked(connection, screen->root, 1, &event_mask.header);

    if (std::unique_ptr<xcb_generic_error_t> error{xcb_request_check(connection, cookie)}) {
      ERROR << f("Couldn't select X11 events for keylogging: %d", error->error_code);
    }
#endif  // __linux__
#ifdef _WIN32
    RegisterRawInput(true);
#endif
  }
  return *keyloggings.emplace_back(new Keylogging(*this, keylogger));
}

enum class CaretAnimAction { Keep, Delete };

static CaretAnimAction DrawCaret(DrawContext& ctx, CaretAnimation& anim, Caret* caret) {
  SkCanvas& canvas = ctx.canvas;
  animation::Context& actx = ctx.animation_context;
  SkPaint paint;
  paint.setColor(SK_ColorBLACK);
  paint.setAntiAlias(true);

  if (caret) {
    SkPath root_shape = caret->MakeRootShape(actx);
    // Animate caret blinking.
    anim.last_blink = caret->last_blink;
    if (anim.shape.isInterpolatable(root_shape)) {
      SkPath out;
      float weight = 1 - anim.delta_fraction.Tick(actx);
      anim.shape.interpolate(root_shape, weight, &out);
      anim.shape = out;
    } else {
      anim.shape = root_shape;
    }
    double now = (actx.timer.now - anim.last_blink).count();
    double seconds, subseconds;
    subseconds = modf(now, &seconds);
    if (subseconds < 0.5) {
      canvas.drawPath(anim.shape, paint);
    }
  } else {
    // Animate disappearance of caret.
    if (anim.keyboard.pointer) {
      SkPath grave = PointerIBeam(anim.keyboard);
      SkPath out;
      float weight = 1 - anim.delta_fraction.Tick(actx);
      anim.shape.interpolate(grave, weight, &out);
      anim.shape = out;
      float dist = (grave.getBounds().center() - anim.shape.getBounds().center()).length();
      if (dist < 0.0001) {
        return CaretAnimAction::Delete;
      }
      canvas.drawPath(anim.shape, paint);
    } else {
      anim.fade_out.target = 1;
      anim.fade_out.Tick(actx);
      paint.setAlphaf(1 - anim.fade_out.value);
      if (paint.getAlphaf() < 0.01) {
        return CaretAnimAction::Delete;
      }
      anim.shape.offset(0, actx.timer.d * kLetterSize);
      canvas.drawPath(anim.shape, paint);
    }
  }
  return CaretAnimAction::Keep;
}

CaretAnimation::CaretAnimation(const Keyboard& keyboard)
    : keyboard(keyboard),
      delta_fraction(50),
      shape(PointerIBeam(keyboard)),
      last_blink(time::SystemNow()) {}

void Keyboard::Draw(DrawContext& ctx) const {
  SkCanvas& canvas = ctx.canvas;
  animation::Context& actx = ctx.animation_context;
  // Iterate through each Caret & CaretAnimation, and draw them.
  // After a Caret has been removed, its CaretAnimation is kept around for some
  // time to animate it out.
  auto& anim_carets = anim[actx].carets;
  auto anim_it = anim_carets.begin();
  auto caret_it = carets.begin();
  while (anim_it != anim_carets.end() && caret_it != carets.end()) {
    if (anim_it->first < caret_it->get()) {
      // Caret was removed.
      auto a = DrawCaret(ctx, anim_it->second, nullptr);
      if (a == CaretAnimAction::Delete) {
        anim_it = anim_carets.erase(anim_it);
      } else {
        ++anim_it;
      }
    } else if (anim_it->first > caret_it->get()) {
      // Caret was added.
      auto new_it =
          anim_carets.emplace(std::make_pair<Caret*, CaretAnimation>(caret_it->get(), *this)).first;
      DrawCaret(ctx, new_it->second, caret_it->get());
      ++caret_it;
    } else {
      DrawCaret(ctx, anim_it->second, caret_it->get());
      ++anim_it;
      ++caret_it;
    }
  }
  while (anim_it != anim_carets.end()) {
    // Caret at end was removed.
    auto a = DrawCaret(ctx, anim_it->second, nullptr);
    if (a == CaretAnimAction::Delete) {
      anim_it = anim_carets.erase(anim_it);
    } else {
      ++anim_it;
    }
  }
  while (caret_it != carets.end()) {
    // Caret at end was added.
    auto new_it =
        anim_carets.emplace(std::make_pair<Caret*, CaretAnimation>(caret_it->get(), *this)).first;
    DrawCaret(ctx, new_it->second, caret_it->get());
    ++caret_it;
  }
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
  KeyDown(key);
}
void Keyboard::KeyDown(xcb_input_raw_key_press_event_t& ev) {
  gui::Key key = {
      .physical = x11::X11KeyCodeToKey((x11::KeyCode)ev.detail),
      .logical = x11::X11KeyCodeToKey((x11::KeyCode)ev.detail),
  };
  LogKeyDown(key);
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
#endif  // __linux__

void Keyboard::KeyDown(Key key) {
  // Quit on Ctrl + Q
  if (key.ctrl && key.physical == AnsiKey::Q) {
    automat_thread.get_stop_source().request_stop();
    return;
  }
  RunOnAutomatThread([=, this]() {
    if (key.physical > AnsiKey::Unknown && key.physical < AnsiKey::Count) {
      pressed_keys.set((size_t)key.physical);
    }
    if (grab) {
      // KeyboardGrabber takes over all key events
      grab->grabber.KeyboardGrabberKeyDown(*grab, key);
    } else if (key.physical == AnsiKey::Escape) {
      // Release the carets when Escape is pressed
      for (auto& caret : carets) {
        caret->owner->ReleaseCaret(*caret);
      }
      carets.clear();
    } else {
      for (auto& caret : carets) {
        caret->owner->KeyDown(*caret, key);
      }
    }
  });
}

void Keyboard::KeyUp(Key key) {
  RunOnAutomatThread([=, this]() {
    if (key.physical > AnsiKey::Unknown && key.physical < AnsiKey::Count) {
      pressed_keys.reset((size_t)key.physical);
    }
    if (grab) {
      grab->grabber.KeyboardGrabberKeyUp(*grab, key);
    } else {
      for (auto& caret : carets) {
        if (caret->owner) {
          caret->owner->KeyUp(*caret, key);
        }
      }
    }
  });
}
void Keyboard::LogKeyDown(Key key) {
  RunOnAutomatThread([=, this]() {
    for (auto& keylogging : keyloggings) {
      keylogging->keylogger.KeyloggerKeyDown(key);
    }
  });
}

void Keyboard::LogKeyUp(Key key) {
  RunOnAutomatThread([=, this]() {
    for (auto& keylogging : keyloggings) {
      keylogging->keylogger.KeyloggerKeyUp(key);
    }
  });
}

std::unique_ptr<gui::Keyboard> keyboard;

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

void SendKeyEvent(AnsiKey physical, bool down) {
#if defined(_WIN32)
  INPUT input = {};
  input.type = INPUT_KEYBOARD;
  input.ki.wScan = KeyToScanCode(physical);
  input.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
  SendInput(1, &input, sizeof(INPUT));
#endif
#if defined(__linux__)
  xcb_test_fake_input(connection, down ? XCB_KEY_PRESS : XCB_KEY_RELEASE,
                      (uint8_t)x11::KeyToX11KeyCode(physical), XCB_CURRENT_TIME, screen->root, 0, 0,
                      0);
  xcb_flush(connection);
#endif
}

Caret::Caret(Keyboard& keyboard) : keyboard(keyboard) {}

CaretOwner::~CaretOwner() {
  for (auto caret : carets) {
    caret->owner = nullptr;
  }
}

Caret& Keyboard::RequestCaret(CaretOwner& caret_owner, const Path& widget_path, Vec2 position) {
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
  caret.widget_path = widget_path;
  caret.PlaceIBeam(position);
  caret_owner.carets.emplace_back(&caret);
  return caret;
}

void CaretOwner::KeyDown(Caret& caret, Key) {}
void CaretOwner::KeyUp(Caret& caret, Key) {}

Keyboard::Keyboard(Window& window) : window(window) { window.keyboards.emplace_back(this); }

Keyboard::~Keyboard() {
  auto it = std::find(window.keyboards.begin(), window.keyboards.end(), this);
  if (it != window.keyboards.end()) {
    window.keyboards.erase(it);
  }
}

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
  grabber.ReleaseGrab(*this);
  keyboard.grab.reset();  // KeyboardGrab deletes itself here!
}

void KeyGrab::Release() {
#if defined(_WIN32)
  if (cb) {
    cb->grab = nullptr;
    cb = nullptr;
  }
  RunOnWindowsThread([id = id]() {
    bool success = UnregisterHotKey(main_window, id);
    if (!success) {
      ERROR << GetLastErrorStr();
    }
  });
#else
  xcb_keycode_t keycode = (U8)x11::KeyToX11KeyCode(key);

  auto cookie = xcb_ungrab_key_checked(connection, keycode, screen->root, XCB_MOD_MASK_ANY);
  if (auto err = xcb_request_check(connection, cookie)) {
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

    xcb_void_cookie_t cookie =
        xcb_input_xi_select_events_checked(connection, screen->root, 1, &event_mask.header);

    if (std::unique_ptr<xcb_generic_error_t> error{xcb_request_check(connection, cookie)}) {
      ERROR << f("Couldn't release X11 event selection: %d", error->error_code);
    }
#endif  // __linux__
#ifdef _WIN32
    RegisterRawInput(false);
#endif
  }
  keyboard.keyloggings.erase(it);  // After this line `this` is deleted!
}

}  // namespace automat::gui
