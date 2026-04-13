// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
#include "window_watch.hh"

#include "root_widget.hh"
#include "window.hh"

namespace automat::ui {

void WindowWatching::Release() {
  auto it = window.window_watchings.begin();
  for (; it != window.window_watchings.end(); ++it) {
    if (it->get() == this) {
      break;
    }
  }
  if (it == window.window_watchings.end()) {
    return;
  }
  watcher.WindowWatcherOnRelease(*this);
  // local variable because `this` is deleted after `window.window_watchings.erase(it)`.
  auto& win = window;
  win.window_watchings.erase(it);
  win.OnWindowWatchingChanged();
}

}  // namespace automat::ui
