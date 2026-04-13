// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>

#include "time.hh"

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

// Ask the OS / window manager to bring `window` to the foreground.
//
// Non-blocking and safe to call from any thread. The request may be ignored,
// honoured immediately, or downgraded to an attention hint depending on the
// window manager's focus-stealing policy. Callers that need to react to the
// outcome should observe foreground changes via the WindowWatching API.
void ActivateWindow(WindowHandle window);

}  // namespace automat::os