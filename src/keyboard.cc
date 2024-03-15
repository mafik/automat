#include "keyboard.hh"

#include <map>
#include <set>

#include "font.hh"
#include "window.hh"

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
  last_blink = time::now();
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
                                  bool shift, bool windows) {
  auto key_grab = std::make_unique<KeyGrab>(*this, key_grabber, key, ctrl, alt, shift, windows);
  key_grabs.emplace_back(std::move(key_grab));
  return *key_grabs.back();
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
      last_blink(time::now()) {}

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

void Keyboard::KeyDown(Key key) {
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

void KeyboardGrab::Release() {
  grabber.ReleaseGrab(*this);
  keyboard.grab.reset();
}

}  // namespace automat::gui
