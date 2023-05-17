#include "keyboard_impl.h"

#include "font.h"
#include "pointer_impl.h"

namespace automaton::gui {

CaretImpl::CaretImpl(KeyboardImpl &keyboard)
    : facade(*this), keyboard(keyboard) {}

CaretImpl::~CaretImpl() {}

static SkPath PointerIBeam(const KeyboardImpl &keyboard) {
  if (keyboard.pointer) {
    float px = 1 / keyboard.window.PxPerMeter();
    vec2 pos = keyboard.pointer->PositionWithin(*root_machine);
    SkRect bounds = SkRect::MakeXYWH(pos.X, pos.Y, 0, 0);
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

void CaretImpl::PlaceIBeam(vec2 position) {
  float width = GetFont().line_thickness;
  float height = kLetterSize;
  shape = SkPath::Rect(
      SkRect::MakeXYWH(position.X - width / 2, position.Y, width, height));
  last_blink = time::now();
}

SkPath CaretImpl::MakeRootShape() const {
  SkMatrix text_to_root =
      root_machine->TransformFromChild(owner->CaretWidget());
  return shape.makeTransform(text_to_root);
}

KeyboardImpl::KeyboardImpl(WindowImpl &window, Keyboard &facade)
    : window(window), facade(facade) {
  window.keyboards.emplace_back(this);
}

KeyboardImpl::~KeyboardImpl() {
  auto it = std::find(window.keyboards.begin(), window.keyboards.end(), this);
  if (it != window.keyboards.end()) {
    window.keyboards.erase(it);
  }
}

enum class CaretAnimAction { Keep, Delete };

static CaretAnimAction DrawCaret(SkCanvas &canvas, CaretAnimation &anim,
                                 CaretImpl *caret,
                                 animation::State &animation_state) {

  SkPaint paint;
  paint.setColor(SK_ColorBLACK);
  paint.setAntiAlias(true);

  if (caret) {
    SkPath root_shape = caret->MakeRootShape();
    // Animate caret blinking.
    anim.last_blink = caret->last_blink;
    if (anim.shape.isInterpolatable(root_shape)) {
      SkPath out;
      float weight = 1 - anim.delta_fraction.Tick(animation_state);
      anim.shape.interpolate(root_shape, weight, &out);
      anim.shape = out;
    } else {
      anim.shape = root_shape;
    }
    double now = (animation_state.timer.now - anim.last_blink).count();
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
      float weight = 1 - anim.delta_fraction.Tick(animation_state);
      anim.shape.interpolate(grave, weight, &out);
      anim.shape = out;
      float dist =
          (grave.getBounds().center() - anim.shape.getBounds().center())
              .length();
      if (dist < 0.0001) {
        return CaretAnimAction::Delete;
      }
      canvas.drawPath(anim.shape, paint);
    } else {
      anim.fade_out.target = 1;
      anim.fade_out.Tick(animation_state);
      paint.setAlphaf(1 - anim.fade_out.value);
      if (paint.getAlphaf() < 0.01) {
        return CaretAnimAction::Delete;
      }
      anim.shape.offset(0, animation_state.timer.d * kLetterSize);
      canvas.drawPath(anim.shape, paint);
    }
  }
  return CaretAnimAction::Keep;
}

CaretAnimation::CaretAnimation(const KeyboardImpl &keyboard)
    : keyboard(keyboard), delta_fraction(50), shape(PointerIBeam(keyboard)),
      last_blink(time::now()) {}

void KeyboardImpl::Draw(SkCanvas &canvas,
                        animation::State &animation_state) const {
  // Iterate through each Caret & CaretAnimation, and draw them.
  // After a Caret has been removed, its CaretAnimation is kept around for some
  // time to animate it out.
  auto &anim_carets = anim[animation_state].carets;
  auto anim_it = anim_carets.begin();
  auto caret_it = carets.begin();
  while (anim_it != anim_carets.end() && caret_it != carets.end()) {
    if (anim_it->first < caret_it->get()) {
      // Caret was removed.
      auto a = DrawCaret(canvas, anim_it->second, nullptr, animation_state);
      if (a == CaretAnimAction::Delete) {
        anim_it = anim_carets.erase(anim_it);
      } else {
        ++anim_it;
      }
    } else if (anim_it->first > caret_it->get()) {
      // Caret was added.
      auto new_it = anim_carets
                        .emplace(std::make_pair<CaretImpl *, CaretAnimation>(
                            caret_it->get(), *this))
                        .first;
      DrawCaret(canvas, new_it->second, caret_it->get(), animation_state);
      ++caret_it;
    } else {
      DrawCaret(canvas, anim_it->second, caret_it->get(), animation_state);
      ++anim_it;
      ++caret_it;
    }
  }
  while (anim_it != anim_carets.end()) {
    // Caret at end was removed.
    auto a = DrawCaret(canvas, anim_it->second, nullptr, animation_state);
    if (a == CaretAnimAction::Delete) {
      anim_it = anim_carets.erase(anim_it);
    } else {
      ++anim_it;
    }
  }
  while (caret_it != carets.end()) {
    // Caret at end was added.
    auto new_it = anim_carets
                      .emplace(std::make_pair<CaretImpl *, CaretAnimation>(
                          caret_it->get(), *this))
                      .first;
    DrawCaret(canvas, new_it->second, caret_it->get(), animation_state);
    ++caret_it;
  }
}

void KeyboardImpl::KeyDown(Key key) {
  RunOnAutomatonThread([=]() {
    if (key.physical > AnsiKey::Unknown && key.physical < AnsiKey::Count) {
      pressed_keys.set((size_t)key.physical);
    }
    if (key.physical == AnsiKey::Escape) {
      for (auto &caret : carets) {
        caret->owner->ReleaseCaret(caret->facade);
      }
      carets.clear();
    } else {
      for (auto &caret : carets) {
        caret->owner->KeyDown(caret->facade, key);
      }
    }
  });
}

void KeyboardImpl::KeyUp(Key key) {
  RunOnAutomatonThread([=]() {
    if (key.physical > AnsiKey::Unknown && key.physical < AnsiKey::Count) {
      pressed_keys.reset((size_t)key.physical);
    }
    for (auto &caret : carets) {
      if (caret->owner) {
        caret->owner->KeyUp(caret->facade, key);
      }
    }
  });
}

} // namespace automaton::gui
