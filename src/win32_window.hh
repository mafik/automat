// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#pragma push_macro("ERROR")
#include <windows.h>
#pragma pop_macro("ERROR")

#include "time.hh"
#include "window.hh"

struct Win32Window : automat::ui::Window {
  HWND hwnd = 0;
  bool keylogging_enabled = false;
  bool window_active = false;
  Vec2 mouse_position;  // mouse position in screen coordinates
  int client_x = 0;
  int client_y = 0;

  Vec2 mouse_logger_last;  // used to convert absolute mouse position to relative movements
  automat::time::SteadyPoint mouse_logger_last_time = automat::time::kZeroSteady;

  ~Win32Window();

  static std::unique_ptr<automat::ui::Window> Make(automat::ui::RootWidget&, automat::Status&);

  void MainLoop() override;
  automat::ui::Pointer& GetMouse() override;
  Vec2 ScreenToWindowPx(Vec2 screen) override;
  Vec2 WindowPxToScreen(Vec2 window) override;
  automat::Optional<Vec2> MousePositionScreenPx() override;
  void RequestResize(Vec2 size) override;
  void RequestMaximize(bool horizontal, bool vertical) override;

  // Windows-specific functions

  void PostToMainLoop(std::function<void()>);
  void OnRegisterInput(bool keylogging, bool pointerlogging) override;

 private:
  Win32Window(automat::ui::RootWidget& root);
};
