#pragma once

#include <include/core/SkCanvas.h>

namespace automat {

namespace gui {
struct Pointer;
struct DrawContext;
struct Widget;
}  // namespace gui

// TODO: maybe merge Begin & End into constructor & destructor
struct Action {
  gui::Pointer& pointer;
  Action(gui::Pointer& pointer) : pointer(pointer) {}
  virtual ~Action() = default;
  virtual void Begin() = 0;
  virtual void Update() = 0;
  virtual void End() = 0;
  virtual gui::Widget* Widget() { return nullptr; }
};

}  // namespace automat
