#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include <atomic>
#include <cstdint>
#include <mutex>

#include "ptr.hpp"
#include "vec.hpp"

namespace automat {

struct Location;
struct Board;

// Groups some Engine-level obects. Exists for purely estetic reasons - each of those could as well
// be a global variable.
//
// There is some similarity with this struct & regular Objects:
// - both have wake counter & toys (for Engine those are "RootWidgets")
// - both could theoretically have interfaces
//
// However the memory managment story is completely different - so it's not treated as a regular
// Object.
struct Engine {
  std::atomic<uint32_t> monitor = 0;

  // Guards `boards` and every Board::locations. Recursive because board mutations can happen
  // inside argument connection callbacks that run under an outer lock (ConnectAtPoint).
  std::recursive_mutex mutex;

  // Boards in front-to-back order.
  Vec<Ptr<Board>> boards;

  void WakeToys() { monitor.fetch_add(1, std::memory_order_relaxed); }
};

extern Engine engine;

// The board where externally-created objects land when no better board is known.
// Creates a board at the origin when none exists.
Board& DefaultBoard();

}  // namespace automat
