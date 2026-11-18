#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Algorithms for selecting stable addresses - that mostly survive changes (maps & unmaps) in the
// address space.

#include <bit>
#include <cstdint>

#include "span.hpp"

namespace automat {

constexpr uint64_t LandmarkToAddress(uint64_t index) {
  if (index == 0) return 0;
  int m = std::bit_width(index) - 1;
  int level = 63 - m;
  return (2 * (index - (1ull << m)) + 1) << level;
}

constexpr uint64_t AddressToLandmark(uint64_t address) {
  if (address == 0) return 0;
  int level = std::countr_zero(address);
  return (1ull << (63 - level)) + (address >> level >> 1);
}

struct AddressRange {
  uint64_t begin, end;
};

// Selects some "landmark" addresses within the given address ranges that should survive across
// changes such as maps & unmaps.
//
// Address ranges must be increasing, non-overlapping and ideally without 0-byte gaps.
//
// Returns the number of selected landmarks (almost always == landmarks.size()).
Size SelectMemoryLandmarks(Span<const AddressRange> ranges, Span<uint64_t> landmarks);

}  // namespace automat
