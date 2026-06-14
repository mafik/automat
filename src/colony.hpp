#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include <memory>

#include "plf_colony.h"

namespace automat {

// `automat::Colony` is plf::colony: an unordered container whose element addresses
// stay valid across insert and erase. See plf_colony.h (notably get_iterator, which
// turns a stored element pointer back into an iterator for O(1) erasure).
template <typename T, typename Allocator = std::allocator<T>>
using Colony = plf::colony<T, Allocator>;

}  // namespace automat
