// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <memory>
#include <mutex>
#include <stop_token>

#include "colony.hpp"
#include "keyboard.hpp"
#include "math.hpp"
#include "pointer.hpp"
#include "window_watch.hpp"

namespace automat::ui {

struct RootWidget;

struct Window {
  RootWidget& root;
  Vec2 vk_size = Vec2(-1, -1);
  int client_width;  // pixels
  int client_height;
  int screen_refresh_rate = 60;
  std::unique_ptr<Pointer> mouse;
  // `mouse` while it's outside the window
  std::unique_ptr<Pointer> mouse_away;

  Pointer& GetMouse();  // un-parks `mouse_away` or creates a new Pointer
  Pointer* MouseOrNull() { return mouse ? mouse.get() : mouse_away.get(); }

  virtual ~Window() = default;

  virtual void MainLoop(std::stop_token) = 0;

  virtual std::unique_ptr<Pointer> MakeMouse() = 0;

  // Converts a point from screen to window pixel coordinates.
  // In pixel coordinates the origin is at the top left and Y goes down.
  virtual Vec2 ScreenToWindowPx(Vec2 screen) = 0;

  // Converts a point from window to screen pixel coordinates.
  // In pixel coordinates the origin is at the top left and Y goes down.
  virtual Vec2 WindowPxToScreen(Vec2 window) = 0;

  virtual Optional<Vec2> MousePositionScreenPx() = 0;

  virtual void RequestResize(Vec2 new_size) = 0;
  virtual void RequestMaximize(bool maximize_horizontally, bool maximize_vertically) = 0;
  virtual void RequestMinimizeToTray() = 0;
  virtual void RequestRestoreFromTray() = 0;

  std::lock_guard<std::mutex> Lock();

  // Ask the OS to deliver the right input events.
  virtual void OnRegisterInput(bool keylogging, bool pointerlogging) = 0;

  void RegisterInput();

  void BeginLogging(Keylogger* keylogger, Keylogging** keylogging, Pointer::Logger* pointer_logger,
                    Pointer::Logging** pointer_logging);

  // Window watching - for tracking foreground window changes.
  Colony<WindowWatching> window_watchings;

  void BeginWindowWatching(WindowWatcher* watcher, WindowWatching** watching);

  // Called when window watchings are added/removed. Platform implementations should
  // subscribe/unsubscribe from OS events as needed.
  virtual void OnWindowWatchingChanged() {}

  // Platform implementations call this to notify all watchers.
  void NotifyForegroundChanged(os::WindowHandle window);

 protected:
  Window(RootWidget& root) : root(root) {}

  // TODO: keep reference to vk::Surface and vk::Swapchain here
};

}  // namespace automat::ui
