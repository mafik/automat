// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#ifdef __linux__
#include <xcb/xcb.h>
#endif

#ifdef _WIN32
#pragma push_macro("ERROR")
#include <windows.h>
#pragma pop_macro("ERROR")
#endif

// Cross-platform OS-specific window functionality.
namespace automat::os {

#ifdef __linux__
using WindowHandle = xcb_window_t;
constexpr WindowHandle kNoWindow = 0;  // XCB_WINDOW_NONE
#elif defined(_WIN32)
using WindowHandle = HWND;
constexpr WindowHandle kNoWindow = nullptr;
#endif

}  // namespace automat::os