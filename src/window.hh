#pragma once

#include <include/core/SkCanvas.h>

#include "keyboard.hh"
#include "math.hh"

namespace automat::gui {

struct Keyboard;
struct Pointer;
struct WindowImpl;

struct Window final {
  Window(Vec2 size, float pixels_per_meter, std::string_view initial_state = "");
  ~Window();
  void Resize(Vec2 size);
  void DisplayPixelDensity(float pixels_per_meter);
  void Draw(SkCanvas&);
  std::string_view GetState();

 private:
  std::unique_ptr<WindowImpl> impl;
  friend struct Keyboard;
  friend struct Pointer;
};

}  // namespace automat::gui
