// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT

#include "window.hh"

#include "root_widget.hh"

namespace automat::gui {

std::lock_guard<std::mutex> Window::Lock() { return std::lock_guard(root.mutex); }

void Window::BeginLogging(Keylogger* keylogger, Keylogging** keylogging,
                          Pointer::Logger* pointer_logger, Pointer::Logging** pointer_logging) {
  assert((keylogger != nullptr) == (keylogging != nullptr));
  assert((pointer_logger != nullptr) == (pointer_logging != nullptr));
  assert(keylogger != nullptr || pointer_logger != nullptr);
  if (keylogging != nullptr) {
    *keylogging = root.keyboard.keyloggings.emplace_back(new Keylogging(root.keyboard, *keylogger)).get();
  }
  if (pointer_logging != nullptr) {
    *pointer_logging =
        mouse->loggings.emplace_back(new Pointer::Logging(*mouse, *pointer_logger)).get();
  }
  RegisterInput();
}

void Window::RegisterInput() {
  OnRegisterInput(!root.keyboard.keyloggings.empty(), mouse && !mouse->loggings.empty());
}

}  // namespace automat::gui