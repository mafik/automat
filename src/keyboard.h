#pragma once

#include <memory>

namespace automaton::gui {

struct KeyboardImpl;
struct Window;

enum Key { kKeyUnknown, kKeyW, kKeyA, kKeyS, kKeyD, kKeyCount };

struct Caret final {};

struct Keyboard final {
  Keyboard(Window &);
  ~Keyboard();
  std::unique_ptr<Caret> MakeCaret();

private:
  std::unique_ptr<KeyboardImpl> impl;
  friend struct Window;
};

} // namespace automaton::gui
