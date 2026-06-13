// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT

#include "window.hpp"

#include "root_widget.hpp"

namespace automat::ui {

std::lock_guard<std::mutex> Window::Lock() { return std::lock_guard(root.mutex); }

Pointer& Window::GetMouse() {
  if (!mouse) {
    if (mouse_away) {
      mouse = std::move(mouse_away);
      mouse->Enter();
    } else {
      mouse = MakeMouse();
    }
  }
  return *mouse;
}

void Window::BeginLogging(Keylogger* keylogger, Keylogging** keylogging,
                          Pointer::Logger* pointer_logger, Pointer::Logging** pointer_logging) {
  assert((keylogger != nullptr) == (keylogging != nullptr));
  assert((pointer_logger != nullptr) == (pointer_logging != nullptr));
  assert(keylogger != nullptr || pointer_logger != nullptr);
  if (keylogging != nullptr) {
    *keylogging =
        root.keyboard.keyloggings.emplace_back(new Keylogging(root.keyboard, *keylogger)).get();
  }
  if (pointer_logging != nullptr) {
    Pointer* device = MouseOrNull();
    if (device == nullptr) {
      device = &GetMouse();
    }
    *pointer_logging =
        device->loggings.emplace_back(new Pointer::Logging(*device, *pointer_logger)).get();
  }
  RegisterInput();
}

void Window::RegisterInput() {
  Pointer* device = MouseOrNull();
  OnRegisterInput(!root.keyboard.keyloggings.empty(), device && !device->loggings.empty());
}

void Window::BeginWindowWatching(WindowWatcher* watcher, WindowWatching** watching) {
  assert(watcher != nullptr);
  assert(watching != nullptr);
  *watching = window_watchings.emplace_back(new WindowWatching(*this, *watcher)).get();
  OnWindowWatchingChanged();
}

void Window::NotifyForegroundChanged(os::WindowHandle window) {
  for (auto& w : window_watchings) {
    w->watcher.WindowWatcherForegroundChanged(*w, window);
  }
}

}  // namespace automat::ui
