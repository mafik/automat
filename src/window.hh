// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <memory>
#include <mutex>
#include <stop_token>

#include "keyboard.hh"
#include "math.hh"
#include "pointer.hh"

namespace automat::ui {

struct RootWidget;

struct Window {
  RootWidget& root;
  Vec2 vk_size = Vec2(-1, -1);
  int client_width;  // pixels
  int client_height;
  int screen_refresh_rate = 60;
  std::unique_ptr<Pointer> mouse;

  virtual ~Window() = default;

  virtual void MainLoop(std::stop_token) = 0;

  virtual Pointer& GetMouse() = 0;

  // Converts a point from screen to window pixel coordinates.
  // In pixel coordinates the origin is at the top left and Y goes down.
  virtual Vec2 ScreenToWindowPx(Vec2 screen) = 0;

  // Converts a point from window to screen pixel coordinates.
  // In pixel coordinates the origin is at the top left and Y goes down.
  virtual Vec2 WindowPxToScreen(Vec2 window) = 0;

  virtual Optional<Vec2> MousePositionScreenPx() = 0;

  virtual void RequestResize(Vec2 new_size) = 0;
  virtual void RequestMaximize(bool maximize_horizontally, bool maximize_vertically) = 0;

  std::lock_guard<std::mutex> Lock();

  // Ask the OS to deliver the right input events.
  virtual void OnRegisterInput(bool keylogging, bool pointerlogging) = 0;

  void RegisterInput();

  void BeginLogging(Keylogger* keylogger, Keylogging** keylogging, Pointer::Logger* pointer_logger,
                    Pointer::Logging** pointer_logging);

 protected:
  Window(RootWidget& root) : root(root) {}

  // TODO: keep reference to vk::Surface and vk::Swapchain here
};

}  // namespace automat::ui
