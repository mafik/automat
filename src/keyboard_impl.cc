#include "keyboard_impl.hh"

#include "font.hh"
#include "pointer_impl.hh"

namespace automat::gui {

CaretImpl::CaretImpl(KeyboardImpl& keyboard) : facade(*this), keyboard(keyboard) {}

CaretImpl::~CaretImpl() {}

static SkPath PointerIBeam(const KeyboardImpl& keyboard) {
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

void CaretImpl::PlaceIBeam(Vec2 position) {
  float width = GetFont().line_thickness;
  float height = kLetterSize;
  shape = SkPath::Rect(SkRect::MakeXYWH(position.x - width / 2, position.y, width, height));
  last_blink = time::now();
}

SkPath CaretImpl::MakeRootShape(animation::Context& actx) const {
  auto begin = find(widget_path.begin(), widget_path.end(), root_machine);
  Path sub_path(begin, widget_path.end());
  SkMatrix text_to_root = TransformUp(sub_path, actx);
  return shape.makeTransform(text_to_root);
}

KeyboardImpl::KeyboardImpl(WindowImpl& window, Keyboard& facade) : window(window), facade(facade) {
  window.keyboards.emplace_back(this);
}

KeyboardImpl::~KeyboardImpl() {
  auto it = std::find(window.keyboards.begin(), window.keyboards.end(), this);
  if (it != window.keyboards.end()) {
    window.keyboards.erase(it);
  }
}

enum class CaretAnimAction { Keep, Delete };

static CaretAnimAction DrawCaret(DrawContext& ctx, CaretAnimation& anim, CaretImpl* caret) {
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

CaretAnimation::CaretAnimation(const KeyboardImpl& keyboard)
    : keyboard(keyboard),
      delta_fraction(50),
      shape(PointerIBeam(keyboard)),
      last_blink(time::now()) {}

void KeyboardImpl::Draw(DrawContext& ctx) const {
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
          anim_carets.emplace(std::make_pair<CaretImpl*, CaretAnimation>(caret_it->get(), *this))
              .first;
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
        anim_carets.emplace(std::make_pair<CaretImpl*, CaretAnimation>(caret_it->get(), *this))
            .first;
    DrawCaret(ctx, new_it->second, caret_it->get());
    ++caret_it;
  }
}

void KeyboardImpl::KeyDown(Key key) {
  RunOnAutomatThread([=]() {
    if (key.physical > AnsiKey::Unknown && key.physical < AnsiKey::Count) {
      pressed_keys.set((size_t)key.physical);
    }
    if (key.physical == AnsiKey::Escape) {
      for (auto& caret : carets) {
        caret->owner->ReleaseCaret(caret->facade);
      }
      carets.clear();
    } else {
      for (auto& caret : carets) {
        caret->owner->KeyDown(caret->facade, key);
      }
    }
  });
}

void KeyboardImpl::KeyUp(Key key) {
  RunOnAutomatThread([=]() {
    if (key.physical > AnsiKey::Unknown && key.physical < AnsiKey::Count) {
      pressed_keys.reset((size_t)key.physical);
    }
    for (auto& caret : carets) {
      if (caret->owner) {
        caret->owner->KeyUp(caret->facade, key);
      }
    }
  });
}

}  // namespace automat::gui
