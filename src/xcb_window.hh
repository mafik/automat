// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <xcb/xcb.h>
#include <xcb/xinput.h>

#include "window.hh"

namespace xcb {

struct VerticalScroll {
  xcb_input_device_id_t device_id;
  uint16_t valuator_number;
  double increment;
  double last_value;
};

struct XCBWindow : automat::gui::Window {
  xcb_window_t xcb_window = 0;

  std::optional<VerticalScroll> vertical_scroll;

  Vec2 window_position_on_screen;
  Vec2 mouse_position_on_screen;

  ~XCBWindow();

  void MainLoop() override;
  automat::gui::Pointer& GetMouse() override;
  Vec2 ScreenToWindowPx(Vec2 screen) override;
  Vec2 WindowPxToScreen(Vec2 window) override;
  maf::Optional<Vec2> MousePositionScreenPx() override;
  void RequestResize(Vec2 new_size) override;
  void RequestMaximize(bool horizontally, bool vertically) override;
  static std::unique_ptr<automat::gui::Window> Make(automat::gui::RootWidget&, maf::Status&);

 protected:
  XCBWindow(automat::gui::RootWidget& root) : automat::gui::Window(root) {}
};

}  // namespace xcb