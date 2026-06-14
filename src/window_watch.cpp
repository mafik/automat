// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
#include "window_watch.hpp"

#include "window.hpp"

namespace automat::ui {

void WindowWatching::Release() {
  watcher.WindowWatcherOnRelease(*this);
  // local variable because `this` is deleted by the erase below.
  auto& win = window;
  win.window_watchings.erase(win.window_watchings.get_iterator(this));
  win.OnWindowWatchingChanged();
}

}  // namespace automat::ui
