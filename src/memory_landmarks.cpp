// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include "memory_landmarks.hpp"

#include <algorithm>
#include <cassert>

namespace automat {

static uint64_t ShiftRightRoundingUp(uint64_t address, int level) {
  if (level == 64) return address != 0;
  return (address >> level) + ((address & ((1ull << level) - 1)) != 0);
}

static uint64_t CountMultiples(AddressRange region, int level) {
  return ShiftRightRoundingUp(region.end, level) - ShiftRightRoundingUp(region.begin, level);
}

static uint64_t CountMultiples(Span<const AddressRange> regions, int level) {
  uint64_t count = 0;
  for (auto& region : regions) count += CountMultiples(region, level);
  return count;
}

static uint64_t* WriteMultiples(uint64_t begin, uint64_t end, int level, uint64_t* out) {
  if (level == 64) {
    if (begin == 0 && end != 0) *out++ = 0;
    return out;
  }
  uint64_t multiple = ShiftRightRoundingUp(begin, level);
  uint64_t multiples_end = ShiftRightRoundingUp(end, level);
  for (; multiple < multiples_end; ++multiple) *out++ = multiple << level;
  return out;
}

Size SelectMemoryLandmarks(Span<const AddressRange> ranges, Span<uint64_t> landmarks) {
  uint64_t budget = landmarks.size();
  if (budget == 0) return 0;

  uint64_t bytes = 0;
  for (auto& region : ranges) {
    bytes += region.end - region.begin;
  }

  // level = log2(space between landmarks at the upper (fully-filled) level)
  int level = std::bit_width(bytes / budget);
  uint64_t count = CountMultiples(ranges, level);
  while (count > budget) count = CountMultiples(ranges, ++level);
  while (level > 0) {
    uint64_t lower_count = CountMultiples(ranges, level - 1);
    if (lower_count > budget) break;
    --level;
    count = lower_count;
  }

  uint64_t dense_end = 0;
  uint64_t remaining = level > 0 ? budget - count : 0;
  for (auto& region : ranges) {
    if (remaining == 0) break;
    uint64_t odd_multiples = CountMultiples(region, level - 1) - CountMultiples(region, level);
    if (odd_multiples >= remaining) {
      uint64_t first_odd_multiple = ShiftRightRoundingUp(region.begin, level - 1) | 1;
      dense_end = ((first_odd_multiple + 2 * (remaining - 1)) << (level - 1)) + 1;
      break;
    }
    remaining -= odd_multiples;
  }

  uint64_t* out = landmarks.data();
  for (auto& region : ranges) {
    uint64_t split = std::clamp(dense_end, region.begin, region.end);
    if (level > 0) out = WriteMultiples(region.begin, split, level - 1, out);
    out = WriteMultiples(split, region.end, level, out);
  }
  return out - landmarks.data();
}

}  // namespace automat
