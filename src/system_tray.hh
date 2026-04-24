// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

namespace automat {

void InitSystemTray();

#ifdef _WIN32

#ifndef WM_APP
#define WM_APP 0x8000
#endif
// Callback message id that the tray icon posts to the main HWND.
constexpr unsigned kSystemTrayMessage = WM_APP + 1;

// Invoked by Win32Window::WndProc for each tray notification.
void OnSystemTrayMessage(unsigned event, int mouse_screen_x, int mouse_screen_y);
#endif

}  // namespace automat
