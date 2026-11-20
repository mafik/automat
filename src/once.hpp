#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include <atomic>

#include "int.hpp"

namespace automat {

struct Once {
  std::atomic<U8> used = 0;

  bool First() { return !used.exchange(1, std::memory_order_relaxed); }
};

static_assert(sizeof(Once) == 1);

}  // namespace automat
