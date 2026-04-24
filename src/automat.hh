// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <thread>

#include "base.hh"  // IWYU pragma: export
#include "vm.hh"    // IWYU pragma: export

// High-level automat code.

namespace automat {

extern std::stop_source stop_source;

extern std::thread::id main_thread_id;

}  // namespace automat
