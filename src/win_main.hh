#pragma once

// Top-level Windows functions.

#include <Windows.h>

#include <functional>

LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR pCmdLine, int nCmdShow);

extern HWND main_window;
extern int client_width;
extern int client_height;

void RunOnWindowsThread(std::function<void()>&&);

void RegisterRawInput(bool keylogging = false);
