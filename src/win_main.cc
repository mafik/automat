#undef NOGDI
#include <windows.h>
#undef ERROR
#define NOGDI

#include "win_main.h"

#include "backtrace.h"
#include "gui.h"
#include "loading_animation.h"
#include "root.h"
#include "vk.h"
#include "win.h"
#include "library.h"

#include <include/core/SkCanvas.h>
#include <include/core/SkGraphics.h>

#include <memory>

using namespace automaton;

HWND main_window;

// This is based on the scaling configured by the user in Windows settings.
// It's not used anywhere by Automaton.
int main_window_dpi = USER_DEFAULT_SCREEN_DPI;

int client_x;
int client_y;
int window_width;
int window_height;

// Placeholder values for screen size. They should be updated when window is
// resized.
int screen_width_px = 1920;
int screen_height_px = 1080;
float screen_width_m =
    (float)screen_width_px / USER_DEFAULT_SCREEN_DPI * kMetersPerInch;
float screen_height_m =
    (float)screen_height_px / USER_DEFAULT_SCREEN_DPI * kMetersPerInch;

vec2 mouse_position;

float DisplayPxPerMeter() { return screen_width_px / screen_width_m; }
vec2 WindowSize() {
  return Vec2(window_width, window_height) / DisplayPxPerMeter();
}

gui::Key ScanCodeToKey(uint8_t scan_code) {
  switch (scan_code) {
  case 0x11:
    return gui::kKeyW;
  case 0x1e:
    return gui::kKeyA;
  case 0x1f:
    return gui::kKeyS;
  case 0x20:
    return gui::kKeyD;
  default:
    return gui::kKeyUnknown;
  }
}

// Coordinate spaces:
// - Screen (used by Windows & win_main.cc, origin in the top left corner of the
// screen, Y axis goes down)
// - Window (used for win_main.cc <=> gui.cc communication, origin in the bottom
// left corner of the window, Y axis goes up)
// - Canvas (used for gui.cc <=> Automaton communication, origin in the center
// of the window, Y axis goes up)

vec2 ScreenToWindow(vec2 screen) {
  vec2 window =
      (screen - Vec2(client_x, client_y + window_height)) / DisplayPxPerMeter();
  window.Y = -window.Y;
  return window;
}

/* TODO: figure out an API for custom cursors

Cursor current_cursor = kCursorUnknown;

void UpdateCursor() {
  Cursor wanted = prototype_under_mouse ? kCursorHand : kCursorArrow;
  if (current_cursor != wanted) {
    current_cursor = wanted;
    SetCursor(current_cursor);
  }
}
*/

std::unique_ptr<gui::Window> window;
std::unique_ptr<gui::Pointer> mouse;

gui::Pointer &GetMouse() {
  if (!mouse) {
    mouse = std::make_unique<gui::Pointer>(*window, ScreenToWindow(mouse_position));
  }
  return *mouse;
}

void ResizeVulkan() {
  if (auto err = vk::Resize(window_width, window_height); !err.empty()) {
    MessageBox(nullptr, err.c_str(), "ALERT", 0);
  }
}

bool tracking_mouse_leave = false;

void TrackMouseLeave() {
  if (tracking_mouse_leave) {
    return;
  }
  tracking_mouse_leave = true;
  TRACKMOUSEEVENT track_mouse_event = {
      .cbSize = sizeof(TRACKMOUSEEVENT),
      .dwFlags = TME_LEAVE,
      .hwndTrack = main_window,
  };
  TrackMouseEvent(&track_mouse_event);
}

void Paint(SkCanvas &canvas) {
  canvas.save();
  canvas.translate(0, window_height);
  canvas.scale(DisplayPxPerMeter(), -DisplayPxPerMeter());
  if (window) {
    window->Draw(canvas);
  }
  canvas.restore();
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  switch (uMsg) {
  case WM_SIZE:
    window_width = LOWORD(lParam);
    window_height = HIWORD(lParam);
    ResizeVulkan();
    if (window) {
      window->Resize(WindowSize());
    }
    break;
  case WM_MOVE: {
    client_x = LOWORD(lParam);
    client_y = HIWORD(lParam);
    break;
  }
  /*
  case WM_SETCURSOR:
    // Intercept this message to prevent Windows from changing the cursor back
    // to an arrow.
    if (LOWORD(lParam) == HTCLIENT) {
      UpdateCursor();
      return TRUE;
    } else {
      current_cursor = kCursorUnknown;
      return DefWindowProc(hWnd, uMsg, wParam, lParam);
    }
    break;
  */
  case WM_PAINT: {
    PAINTSTRUCT ps;
    BeginPaint(hWnd, &ps);
    SkCanvas *canvas = vk::GetBackbufferCanvas();
    if (anim) {
      anim.OnPaint(*canvas, Paint);
    } else {
      Paint(*canvas);
    }
    vk::Present();
    EndPaint(hWnd, &ps);
    break;
  }
  case WM_DPICHANGED: {
    main_window_dpi = HIWORD(wParam);
    RECT *const size_hint = (RECT *)lParam;
    SetWindowPos(hWnd, NULL, size_hint->left, size_hint->top,
                 size_hint->right - size_hint->left,
                 size_hint->bottom - size_hint->top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    break;
  }
  case WM_KEYDOWN: {
    uint8_t key = (uint8_t)wParam;             // layout-dependent key code
    uint8_t scan_code = (lParam >> 16) & 0xFF; // identifies the physical key
    if (window) {
      window->KeyDown(ScanCodeToKey(scan_code));
    }
    break;
  }
  case WM_KEYUP: {
    uint8_t key = (uint8_t)wParam;
    uint8_t scan_code = (lParam >> 16) & 0xFF;
    if (window) {
      window->KeyUp(ScanCodeToKey(scan_code));
    }
    break;
  }
  case WM_CHAR: {
    uint8_t utf8_char = (uint8_t)wParam;
    uint8_t scan_code = (lParam >> 16) & 0xFF;
    break;
  }
  case WM_LBUTTONDOWN: {
    GetMouse().ButtonDown(gui::kMouseLeft);
    TrackMouseLeave();
    break;
  }
  case WM_LBUTTONUP: {
    GetMouse().ButtonUp(gui::kMouseLeft);
    break;
  }
  case WM_MBUTTONDOWN: {
    GetMouse().ButtonDown(gui::kMouseMiddle);
    TrackMouseLeave();
    break;
  }
  case WM_MBUTTONUP: {
    GetMouse().ButtonUp(gui::kMouseMiddle);
    break;
  }
  case WM_MOUSEMOVE: {
    int16_t x = lParam & 0xFFFF;
    int16_t y = (lParam >> 16) & 0xFFFF;
    mouse_position.X = x + client_x;
    mouse_position.Y = y + client_y;
    GetMouse().Move(ScreenToWindow(mouse_position));
    break;
  }
  case WM_MOUSELEAVE:
    tracking_mouse_leave = false;
    mouse.reset();
    break;
  case WM_MOUSEWHEEL: {
    int16_t x = lParam & 0xFFFF;
    int16_t y = (lParam >> 16) & 0xFFFF;
    int16_t delta = GET_WHEEL_DELTA_WPARAM(wParam);
    GetMouse().Wheel(delta / 120.0);
    break;
  }
  case WM_DESTROY:
    PostQuitMessage(0);
    break;
  default:
    return DefWindowProc(hWnd, uMsg, wParam, lParam);
    break;
  }
  return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR pCmdLine,
                   int nCmdShow) {
  EnableBacktraceOnSIGSEGV();
  // Switch to UTF-8
  setlocale(LC_CTYPE, ".utf8");
  SetConsoleCP(CP_UTF8);
  SetConsoleOutputCP(CP_UTF8);

  InitRoot();
  anim.LoadingCompleted();

  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  SkGraphics::Init();

  if (!RegisterClassEx(&GetWindowClass())) {
    FATAL() << "Failed to register window class.";
  }

  main_window = CreateAutomatonWindow();
  if (!main_window) {
    FATAL() << "Failed to create main window.";
  }
  main_window_dpi = GetDpiForWindow(main_window);
  HDC hdc = GetDC(main_window);
  screen_width_m = GetDeviceCaps(hdc, HORZSIZE) / 1000.0f;
  screen_height_m = GetDeviceCaps(hdc, VERTSIZE) / 1000.0f;
  screen_width_px = GetDeviceCaps(hdc, HORZRES);
  screen_height_px = GetDeviceCaps(hdc, VERTRES);
  ReleaseDC(main_window, hdc);

  if (auto err = vk::Init(); !err.empty()) {
    FATAL() << "Failed to initialize Vulkan: " << err;
  }

  ShowWindow(main_window, nCmdShow);
  UpdateWindow(main_window);
  RECT rect;
  GetClientRect(main_window, &rect);
  client_x = rect.left;
  client_y = rect.top;
  window_width = rect.right - rect.left;
  window_height = rect.bottom - rect.top;
  window.reset(new gui::Window(WindowSize(), DisplayPxPerMeter()));
  ResizeVulkan();

  MSG msg = {};
  while (WM_QUIT != msg.message) {
    if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    } else {
      // When idle, request a repaint.
      InvalidateRect(main_window, nullptr, false);
    }
  }

  vk::Destroy();
  window.reset(); // delete it manually because destruction from the fini
                  // section happens after global destructors (specifically the
                  // `windows` list).
  return (int)msg.wParam;
}
