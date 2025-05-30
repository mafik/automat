// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <random>

#include "span.hh"

extern std::mt19937 generator;

template <typename T>
T random() {
  std::uniform_int_distribution<T> distr(std::numeric_limits<T>::min(),
                                         std::numeric_limits<T>::max());
  return distr(generator);
}

namespace automat {

// Algorithm "xor" from p. 4 of Marsaglia, "Xorshift RNGs"
struct XorShift32 {
  // The state must be initialized to non-zero
  U32 state = 123456789;

  static XorShift32 MakeFromCurrentTime();

  U32 Roll() {
    U32 x = state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return state = x;
  }

  // Pick a random float in the range [min, max).
  float RollFloat(float min, float max) { return min + (max - min) * Roll() / 4294967296.0f; }
};

static float SeededFloat(float min, float max, double seed) {
  return min + (max - min) * fmod(fabs(sin(seed)) * 43758.5453, 1.0);
}

// This function may block if there is not enough entropy available.
//
// See `man 2 getrandom` for more information.
void RandomBytesSecure(Span<> out);

struct SplitMix64 {
  U64 state;

  SplitMix64(U64 seed) : state(seed) {}

  U64 Next() {
    U64 z = (state += 0x9e3779b97f4a7c15);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9;
    z = (z ^ (z >> 27)) * 0x94d049bb133111eb;
    return z ^ (z >> 31);
  }
};

template <int min_inclusive, int max_exclusive, typename Generator>
int RandomInt(Generator& gen) {
  U64 n = gen.Next();
  return min_inclusive + n % (max_exclusive - min_inclusive);
}

}  // namespace automat