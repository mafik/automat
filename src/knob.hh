#pragma once

#include <deque>

#include "math.hh"
#include "sincos.hh"
#include "units.hh"

namespace automat {

// Turns a gesture into a continuous value.
//
// Allows scrolling and turning in any direction.
struct Knob {
  // Better don't use this.
  std::deque<Vec2> history;

  // Use these to parametrize the knob.
  maf::SinCos unit_angle = maf::SinCos::FromDegrees(45);
  float unit_distance = 5_mm;

  // Use this to read out the current value of the knob.
  float value = 0;

  // Use this to read out the current direction of the values.
  //
  // Initially the values increase towards the right.
  maf::SinCos tangent = maf::SinCos::FromDegrees(0);

  // Use this to read out the current curvature of the values.
  //
  // Initially the values are placed in a straight line.
  float radius = std::numeric_limits<float>::infinity();
  Vec2 center = {0, 0};

  void Update(Vec2 position);
};

}  // namespace automat