#include "keyboard.h"

#include "keyboard_impl.h"
#include "window.h"
#include "window_impl.h"

namespace automaton::gui {

Keyboard::Keyboard(Window &window)
    : impl(std::make_unique<KeyboardImpl>(*window.impl, *this)) {}

Keyboard::~Keyboard() {}

} // namespace automaton::gui
