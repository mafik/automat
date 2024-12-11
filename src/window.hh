// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <memory>

#include "math.hh"
#include "pointer.hh"

namespace automat::gui {

struct RootWidget;

struct Window {
  RootWidget& root;
  Vec2 vk_size = Vec2(-1, -1);
  int client_width;  // pixels
  int client_height;
  int screen_refresh_rate = 60;
  std::unique_ptr<Pointer> mouse;

  virtual ~Window() = default;

  virtual void MainLoop() = 0;

  virtual Pointer& GetMouse() = 0;

  // Converts a point in the screen pixel coordinates (origin at the top left) to window pixel
  // coordinates (origin at the bottom left).
  virtual Vec2 ScreenToWindowPx(Vec2 screen) = 0;

  // Converts a point in the window pixel coordinates (origin at the bottom left) to screen pixel
  // coordinates (origin at the top left).
  virtual Vec2 WindowPxToScreen(Vec2 window) = 0;

  virtual maf::Optional<Vec2> MousePositionScreenPx() = 0;

  virtual void RequestResize(Vec2 new_size) = 0;
  virtual void RequestMaximize(bool maximize_horizontally, bool maximize_vertically) = 0;

  std::lock_guard<std::mutex> Lock();

 protected:
  Window(RootWidget& root) : root(root) {}

  // TODO: keep reference to vk::Surface and vk::Swapchain here
};

}  // namespace automat::gui