#pragma once

// Windows utility functions.

#include <Windows.h>

namespace automaton {

static const wchar_t kWindowClass[] = L"Automaton";
static const wchar_t kWindowTitle[] = L"Automaton";

HINSTANCE GetInstance();
WNDCLASSEX &GetWindowClass();
HWND CreateAutomatonWindow();

extern HWND main_window;

} // namespace automaton
