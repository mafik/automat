#pragma once

#include "animation.h"
#include "math.h"

#include <include/core/SkCanvas.h>

namespace automat {

namespace gui {
struct Pointer;
}

struct Action {
  virtual ~Action() = default;
  virtual void Begin(gui::Pointer &) = 0;
  virtual void Update(gui::Pointer &) = 0;
  virtual void End() = 0;
  virtual void Draw(SkCanvas &canvas, animation::State &animation_state) = 0;
};

} // namespace automat
