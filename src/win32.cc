// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "win32.hh"

#include <timeapi.h>
#include <winuser.h>

#include <clocale>

#include "format.hh"
#include "log.hh"

#pragma comment(lib, "winmm.lib")  // needed for timeBeginPeriod
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shlwapi.lib")

using namespace automat;

namespace automat::win32 {

DisplayCaps caps;

void ProcessSetup() {
  // Switch to UTF-8
  setlocale(LC_CTYPE, ".utf8");
  // This should allow us to write to console even though target subsystem is "windows".
  AttachConsole(ATTACH_PARENT_PROCESS);
  SetConsoleCP(CP_UTF8);
  SetConsoleOutputCP(CP_UTF8);
  // This makes std::this_thread::sleep_until() more accurate.
  timeBeginPeriod(1);
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  caps = DisplayCaps::Query();
}

HINSTANCE GetInstance() {
  static HINSTANCE instance = GetModuleHandle(nullptr);
  return instance;
}

Str GetLastErrorStr() {
  DWORD error = GetLastError();
  if (error == 0) return "No error";
  LPSTR messageBuffer = nullptr;
  size_t size = FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, error, 0, (LPSTR)&messageBuffer, 0, nullptr);
  Str message(messageBuffer, size);
  LocalFree(messageBuffer);
  return message;
}

bool IsMaximized(HWND hWnd) {
  WINDOWPLACEMENT placement = {};
  placement.length = sizeof(WINDOWPLACEMENT);
  GetWindowPlacement(hWnd, &placement);
  return placement.showCmd == SW_SHOWMAXIMIZED;
}

DisplayCaps DisplayCaps::Query() {
  DisplayCaps caps;
  constexpr bool kLogScreenCaps = true;
  {
    caps.screen_left_px = GetSystemMetrics(SM_XVIRTUALSCREEN);
    caps.screen_width_px = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    caps.screen_height_px = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    caps.screen_top_px = GetSystemMetrics(SM_YVIRTUALSCREEN);
    if constexpr (kLogScreenCaps) {
      LOG << "Virtual screen: left=" << caps.screen_left_px << ", top=" << caps.screen_top_px
          << ", " << caps.screen_width_px << "x" << caps.screen_height_px;
    }
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR hMonitor, HDC crappy_hdc, LPRECT rect, LPARAM user_data) {
          DisplayCaps& caps = *(DisplayCaps*)user_data;
          MONITORINFOEX monitor_info = {};
          monitor_info.cbSize = sizeof(monitor_info);
          if (!GetMonitorInfo(hMonitor, &monitor_info)) {
            return TRUE;
          }
          if (!(monitor_info.dwFlags & MONITORINFOF_PRIMARY)) {
            // Skip non-primary monitors
            return TRUE;
          }
          HDC hdc = CreateIC(nullptr, monitor_info.szDevice, nullptr, nullptr);
          if (hdc == 0) {
            return TRUE;
          }
          float monitor_width_m = GetDeviceCaps(hdc, HORZSIZE) / 1000.0f;
          float monitor_height_m = GetDeviceCaps(hdc, VERTSIZE) / 1000.0f;
          float monitor_diagonal_m =
              sqrt(monitor_width_m * monitor_width_m + monitor_height_m * monitor_height_m);
          float monitor_width_px = monitor_info.rcMonitor.right - monitor_info.rcMonitor.left;
          float monitor_height_px = monitor_info.rcMonitor.bottom - monitor_info.rcMonitor.top;
          float monitor_diagonal_px =
              sqrt(monitor_width_px * monitor_width_px + monitor_height_px * monitor_height_px);
          caps.px_per_meter = monitor_diagonal_px / monitor_diagonal_m;
          caps.screen_refresh_rate = GetDeviceCaps(hdc, VREFRESH);
          DeleteDC(hdc);

          if constexpr (kLogScreenCaps) {
            float diag = sqrt(caps.screen_height_m() * caps.screen_height_m() +
                              caps.screen_width_m() * caps.screen_width_m()) /
                         0.0254f;
            LOG << "Display: " << f("%.1f", diag) << "″ " << int(caps.screen_width_m() * 1000)
                << "x" << int(caps.screen_height_m() * 1000) << "mm (" << caps.screen_width_px
                << "x" << caps.screen_height_px << "px) " << caps.screen_refresh_rate << "Hz";
          }

          return TRUE;
        },
        reinterpret_cast<LPARAM>(&caps));
  }
  return caps;
}
}  // namespace automat::win32