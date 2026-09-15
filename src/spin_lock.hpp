#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include <atomic>
#include <thread>

#include "int.hpp"

namespace automat {

struct SpinLock {
  std::atomic<U8> locked = 0;

  void lock() {
    int spins = 0;
    while (locked.exchange(1, std::memory_order_acquire)) {
      do {
        if (++spins > 64) std::this_thread::yield();
      } while (locked.load(std::memory_order_relaxed));
    }
  }
  void unlock() { locked.store(0, std::memory_order_release); }
};

static_assert(sizeof(SpinLock) == 1);

}  // namespace automat
