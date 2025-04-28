#pragma once

#include <span>
#include <vector>

#include "animation.hh"

namespace automat {

struct Wave1D final {
  int n;
  float wave_speed;
  float column_spacing;
  float damping_half_time;
  std::vector<float> state;

  Wave1D(int n, float wave_speed, float column_spacing, float damping_half_time = 0);

  animation::Phase Tick(time::Timer& timer);

  std::span<const float> Amplitudes() const;
  std::span<float> Amplitude();
  std::span<float> Velocity();
  float& operator[](int i) { return Amplitude()[i]; }

  void ZeroMeanAmplitude();
};

}  // namespace automat