// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#pragma push_macro("ERROR")
#include <windows.h>
#pragma pop_macro("ERROR")

#include "window.hh"

struct Win32Window : automat::gui::Window {
  HWND hwnd = 0;
  bool keylogging_enabled = false;
  bool window_active = false;
  Vec2 mouse_position;  // mouse position in screen coordinates
  int client_x = 0;
  int client_y = 0;

  ~Win32Window();

  static std::unique_ptr<automat::gui::Window> Make(automat::gui::RootWidget&, Status&);

  void MainLoop() override;
  automat::gui::Pointer& GetMouse() override;
  Vec2 ScreenToWindowPx(Vec2 screen) override;
  Vec2 WindowPxToScreen(Vec2 window) override;
  Optional<Vec2> MousePositionScreenPx() override;
  void RequestResize(Vec2 size) override;
  void RequestMaximize(bool horizontal, bool vertical) override;

  // Windows-specific functions

  void PostToMainLoop(std::function<void()>);
  void RegisterRawInput(bool keylogging = false);

 private:
  Win32Window(automat::gui::RootWidget& root);
};
