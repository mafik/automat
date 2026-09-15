// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
#include "engine.hpp"

#include "base.hpp"  // IWYU pragma: keep

namespace automat {

Engine engine;

Board& DefaultBoard() {
  auto lock = std::lock_guard(engine.mutex);
  if (engine.boards.empty()) {
    engine.boards.push_back(MAKE_PTR(Board));
    engine.WakeToys();
  }
  return *engine.boards.front();
}

}  // namespace automat
