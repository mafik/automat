#pragma once

// Windows utility functions.

#include <Windows.h>

namespace automat {

static const char kWindowClass[] = "Automat";
static const char kWindowTitle[] = "Automat";

HINSTANCE GetInstance();
WNDCLASSEX &GetWindowClass();
HWND CreateAutomatWindow();

} // namespace automat
