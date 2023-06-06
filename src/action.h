#pragma once

#include <include/core/SkCanvas.h>

#include "animation.h"
#include "math.h"

namespace automat {

namespace gui {
struct Pointer;
struct DrawContext;
}  // namespace gui

struct Action {
  virtual ~Action() = default;
  virtual void Begin(gui::Pointer&) = 0;
  virtual void Update(gui::Pointer&) = 0;
  virtual void End() = 0;
  virtual void DrawAction(gui::DrawContext&) = 0;
};

}  // namespace automat
