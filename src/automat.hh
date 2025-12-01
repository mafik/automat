// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <thread>

#include "base.hh"

// High-level automat code.

namespace automat {

extern std::stop_source stop_source;
extern Ptr<Location> root_location;
extern Ptr<Machine> root_machine;

extern std::thread::id main_thread_id;

}  // namespace automat
