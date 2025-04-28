#pragma once

#include <span>
#include <vector>

namespace automat {

struct Wave1D final {
  int n;
  float wave_speed;
  float column_spacing;
  std::vector<float> state;

  Wave1D(int n, float wave_speed, float column_spacing);

  void Step(float dt);

  std::span<const float> Amplitudes() const;
  std::span<float> Amplitudes();
  std::span<float> Velocity();
  float& operator[](int i) { return Amplitudes()[i]; }
};

}  // namespace automat