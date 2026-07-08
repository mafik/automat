#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
// Warning: coded with a stochastic parrot

// Platform-neutral entry point for the mux reactor. Every backend defines the same
// automat::mux interface: Epoll, Timer, Init, Stop & WatchProcess.

#if defined(__linux__)
#include "mux_epoll.hpp"  // IWYU pragma: export
#include "mux_timer.hpp"  // IWYU pragma: export
#elif defined(_WIN32)
#include "mux_win32.hpp"  // IWYU pragma: export
#endif
