#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include <cstdlib>
#include <memory>  // IWYU pragma: export

namespace automat {

template <auto unref>
struct DeleteWith {
  void operator()(auto* ptr) const { unref(ptr); }
};

using DeleteWithFree = DeleteWith<free>;

}  // namespace automat
