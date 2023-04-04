#include "keyboard.h"

#include "keyboard_impl.h"
#include "window.h"
#include "window_impl.h"

namespace automaton::gui {

Caret::Caret(Keyboard &keyboard)
    : impl(std::make_unique<CaretImpl>(*keyboard.impl)) {}

Caret::~Caret() {}

void Caret::PlaceIBeam(vec2 canvas_position) {
  impl->PlaceIBeam(canvas_position);
}

Keyboard::Keyboard(Window &window)
    : impl(std::make_unique<KeyboardImpl>(*window.impl, *this)) {}

Keyboard::~Keyboard() {}

void Keyboard::Draw(SkCanvas &canvas, animation::State &animation_state) const {
  impl->Draw(canvas, animation_state);
}

} // namespace automaton::gui
