// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <thread>

#include "base.hpp"  // IWYU pragma: export
#include "vm.hpp"    // IWYU pragma: export

// High-level automat code.

namespace automat {

extern std::stop_source stop_source;

extern std::thread::id main_thread_id;

}  // namespace automat
