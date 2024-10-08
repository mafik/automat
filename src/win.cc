// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "win.hh"

#include "win_main.hh"
#include "window.hh"

using namespace automat::gui;

namespace automat {

HINSTANCE GetInstance() {
  static HINSTANCE instance = GetModuleHandle(nullptr);
  return instance;
}

WNDCLASSEX& GetWindowClass() {
  static WNDCLASSEX wcex = []() {
    WNDCLASSEX wcex = {.cbSize = sizeof(WNDCLASSEX),
                       .style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC,
                       .lpfnWndProc = WndProc,
                       .cbClsExtra = 0,
                       .cbWndExtra = 0,
                       .hInstance = GetInstance(),
                       .hIcon = LoadIcon(GetInstance(), (LPCTSTR)IDI_WINLOGO),
                       .hCursor = LoadCursor(nullptr, IDC_ARROW),
                       .hbrBackground = (HBRUSH)(COLOR_WINDOW + 1),
                       .lpszMenuName = nullptr,
                       .lpszClassName = kWindowClass,
                       .hIconSm = LoadIcon(GetInstance(), (LPCTSTR)IDI_WINLOGO)};
    return wcex;
  }();
  return wcex;
}

maf::Str GetLastErrorStr() {
  DWORD error = GetLastError();
  if (error == 0) return "No error";
  LPSTR messageBuffer = nullptr;
  size_t size = FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, error, 0, (LPSTR)&messageBuffer, 0, nullptr);
  maf::Str message(messageBuffer, size);
  LocalFree(messageBuffer);
  return message;
}

}  // namespace automat