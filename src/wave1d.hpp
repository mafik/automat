#pragma once
// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include <span>
#include <vector>

#include "widget.hpp"

namespace automat {

struct Wave1D final {
  int n;
  float wave_speed;
  float column_spacing;
  float damping_half_time;
  std::vector<float> state;

  Wave1D(int n, float wave_speed, float column_spacing, float damping_half_time = 0);

  ui::Tick Tock(time::Timer& timer);

  std::span<const float> Amplitudes() const;
  std::span<float> Amplitude();
  std::span<float> Velocity();
  float& operator[](int i) { return Amplitude()[i]; }

  void ZeroMeanAmplitude();
};

}  // namespace automat