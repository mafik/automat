// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstdint>

#include "ptr.hh"

namespace automat {

struct Location;
struct Board;

// Groups some VM-level obects. Exists for purely estetic reasons - each of those could as well be a
// global variable.
//
// There is some similarity with this struct & regular Objects:
// - both have wake counter & toys (for VM those are "RootWidgets")
// - both could theoretically have interfaces
//
// However the memory managment story is completely different - so it's not treated as a regular
// Object.
struct VM {
  std::atomic<uint32_t> wake_counter = 0;

  Ptr<Location> root_location;
  Ptr<Board> root_board;

  void WakeToys() { wake_counter.fetch_add(1, std::memory_order_relaxed); }
};

extern VM vm;

}  // namespace automat
