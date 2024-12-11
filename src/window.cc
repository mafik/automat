// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT

#include "window.hh"

#include "root_widget.hh"

namespace automat::gui {

std::lock_guard<std::mutex> Window::Lock() { return std::lock_guard(root.mutex); }

}  // namespace automat::gui