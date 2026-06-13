// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
#include "os_window.hpp"

#include "time.hpp"

#ifdef __linux__
#include "xcb.hpp"
#endif

namespace automat::os {

std::atomic<time::SteadyPoint> last_activated_time = time::kZeroSteady;

void ActivateWindow(WindowHandle window) {
  if (window == kNoWindow) return;
#ifdef __linux__
  xcb::freedesktop::ActivateWindow(window);
#elif defined(_WIN32)
  // SetForegroundWindow is non-blocking and safe from any thread. It may
  // refuse and only flash the taskbar entry — same downgrade behaviour as
  // _NET_ACTIVE_WINDOW under a strict EWMH window manager.
  ::SetForegroundWindow(window);
#endif
}

}  // namespace automat::os
