// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

// Windows utility functions.

#pragma push_macro("ERROR")
#undef NOGDI
#include <windows.h>
#pragma pop_macro("ERROR")

#include "math_constants.hh"
#include "str.hh"

namespace automat::win32 {

void ProcessSetup();

HINSTANCE GetInstance();
maf::Str GetLastErrorStr();

bool IsMaximized(HWND hWnd);

struct DisplayCaps {
  int screen_left_px = 0;
  int screen_top_px = 0;
  int screen_width_px = 1920;
  int screen_height_px = 1080;
  int screen_refresh_rate = 60;
  float px_per_meter = USER_DEFAULT_SCREEN_DPI / kMetersPerInch;

  float screen_width_m() const { return screen_width_px / px_per_meter; }
  float screen_height_m() const { return screen_height_px / px_per_meter; }

  static DisplayCaps Query();
};

extern DisplayCaps caps;

}  // namespace automat::win32
