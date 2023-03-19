#pragma once

#include "math.h"

#include <include/core/SkCanvas.h>

namespace automaton {

struct Action {
  virtual void Begin(vec2 position) = 0;
  virtual void Update(vec2 position) = 0;
  virtual void End() = 0;
  virtual void Draw(SkCanvas& canvas) = 0;
};

} // namespace automaton
