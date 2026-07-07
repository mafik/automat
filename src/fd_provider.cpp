// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "fd_provider.hpp"

#include "object.hpp"

namespace automat {

FdProvider FindFdProvider(Object& obj) {
  FdProvider found;
  obj.Interfaces([&](Interface i) {
    if (auto fd = dyn_cast<FdProvider>(i)) {
      found = fd;
      return LoopControl::Break;
    }
    return LoopControl::Continue;
  });
  return found;
}

}  // namespace automat
