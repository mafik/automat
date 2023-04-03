#pragma once

#include "window_impl.h"

namespace automaton::gui {

struct KeyboardImpl {
  WindowImpl &window;
  Keyboard &facade;
  KeyboardImpl(WindowImpl &window, Keyboard &facade);
  ~KeyboardImpl();
};

} // namespace automaton::gui
