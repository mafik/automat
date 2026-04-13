// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "os_window.hh"

#ifdef __linux__
#include <xcb/xcb.h>
#endif

#ifdef _WIN32
#pragma push_macro("ERROR")
#include <windows.h>
#pragma pop_macro("ERROR")
#endif

namespace automat::ui {

struct Window;

struct WindowWatching;

// Interface for objects that want to receive notifications about foreground window changes.
//
// Similar to Keylogger / Pointer::Logger - objects implement this interface and register
// through ui::Window::BeginWindowWatching.
struct WindowWatcher {
  virtual ~WindowWatcher() = default;

  // Called when the foreground window changes.
  // `window` is the newly focused window handle (or kNoWindow if no window is focused).
  virtual void WindowWatcherForegroundChanged(WindowWatching&, os::WindowHandle window) = 0;

  // Called when the WindowWatching handle is released.
  virtual void WindowWatcherOnRelease(const WindowWatching&) = 0;
};

// Handle representing an active window watch registration. Call Release() to stop watching.
// Similar to Keylogging / Pointer::Logging.
struct WindowWatching {
  Window& window;
  WindowWatcher& watcher;

  WindowWatching(Window& window, WindowWatcher& watcher) : window(window), watcher(watcher) {}

  // Releases this watch. After this call, `this` is deleted.
  void Release();
};

}  // namespace automat::ui
