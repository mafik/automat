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

namespace maf {

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

}  // namespace maf