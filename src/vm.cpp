// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
#include "vm.hpp"

#include "base.hpp"  // IWYU pragma: keep

namespace automat {

VM vm;

Board& DefaultBoard() {
  auto lock = std::lock_guard(vm.mutex);
  if (vm.boards.empty()) {
    vm.boards.push_back(MAKE_PTR(Board));
    vm.WakeToys();
  }
  return *vm.boards.front();
}

}  // namespace automat
