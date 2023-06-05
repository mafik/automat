#include "win.h"

#include "win_main.h"

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

HWND CreateAutomatWindow() {
  return CreateWindowEx(WS_EX_OVERLAPPEDWINDOW, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW,
                        CW_USEDEFAULT, CW_USEDEFAULT, 800, 600, nullptr, nullptr, GetInstance(),
                        nullptr);
}

}  // namespace automat