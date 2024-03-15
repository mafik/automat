#pragma once

// Windows utility functions.

#include <Windows.h>

#include "str.hh"

namespace automat {

static const char kWindowClass[] = "Automat";
static const char kWindowTitle[] = "Automat";

HINSTANCE GetInstance();
WNDCLASSEX& GetWindowClass();
HWND CreateAutomatWindow();
maf::Str GetLastErrorStr();

}  // namespace automat
