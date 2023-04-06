#include "keyboard.h"

#include "keyboard_impl.h"
#include "window.h"
#include "window_impl.h"

namespace automaton::gui {

Caret::Caret(CaretImpl &impl) : impl(impl) {}

void Caret::PlaceIBeam(vec2 canvas_position) {
  impl.PlaceIBeam(canvas_position);
}

CaretOwner::~CaretOwner() {
  for (auto caret : carets) {
    caret->owner = nullptr;
  }
}

Caret &CaretOwner::RequestCaret(Keyboard &keyboard) {
  if (keyboard.impl->carets.empty()) {
    keyboard.impl->carets.emplace_back(
        std::make_unique<CaretImpl>(*keyboard.impl));
  }
  auto &caret = *keyboard.impl->carets.back();
  if (caret.owner) {
    caret.owner->ReleaseCaret(caret.facade);
    caret.owner->carets.erase(std::find(caret.owner->carets.begin(),
                                        caret.owner->carets.end(), &caret));
  }
  caret.owner = this;
  carets.emplace_back(&caret);
  return caret.facade;
}

void CaretOwner::KeyDown(Caret &caret, Key) {}
void CaretOwner::KeyUp(Caret &caret, Key) {}

Keyboard::Keyboard(Window &window)
    : impl(std::make_unique<KeyboardImpl>(*window.impl, *this)) {}

Keyboard::~Keyboard() {}

void Keyboard::Draw(SkCanvas &canvas, animation::State &animation_state) const {
  impl->Draw(canvas, animation_state);
}

void Keyboard::KeyDown(Key key) { impl->KeyDown(key); }
void Keyboard::KeyUp(Key key) { impl->KeyUp(key); }

} // namespace automaton::gui
