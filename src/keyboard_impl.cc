#include "keyboard_impl.h"

namespace automaton::gui {

CaretImpl::CaretImpl(KeyboardImpl &keyboard) : keyboard(keyboard) {
  keyboard.carets.emplace_back(this);
}
CaretImpl::~CaretImpl() {
  auto it = std::find(keyboard.carets.begin(), keyboard.carets.end(), this);
  if (it != keyboard.carets.end()) {
    keyboard.carets.erase(it);
  }
}
void CaretImpl::PlaceIBeam(vec2 canvas_position) {
  shape = SkPath::Rect(
      SkRect::MakeXYWH(canvas_position.X, canvas_position.Y, 0.001f, 0.002f));
}

void CaretImpl::Draw(SkCanvas &canvas,
                     animation::State &animation_state) const {
  SkPaint paint;
  paint.setColor(SK_ColorBLACK);
  canvas.drawPath(shape, paint);
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

void KeyboardImpl::Draw(SkCanvas &canvas,
                        animation::State &animation_state) const {
  for (auto &caret : carets) {
    caret->Draw(canvas, animation_state);
  }
}

} // namespace automaton::gui
