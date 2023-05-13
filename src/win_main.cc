#include <mutex>
#undef NOGDI
#include <windows.h>
#undef ERROR

#include "win_main.h"

#include <atomic>
#include <memory>
#include <thread>

#include <timeapi.h>
#include <winuser.h>

#include <include/core/SkCanvas.h>
#include <include/core/SkGraphics.h>
#include <src/base/SkUTF.h>

#include "backtrace.h"
#include "library.h"
#include "loading_animation.h"
#include "root.h"
#include "touchpad.h"
#include "vk.h"
#include "widget.h"
#include "win.h"
#include "win_key.h"
#include "window.h"

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
int screen_refresh_rate = 60;
float screen_width_m =
    (float)screen_width_px / USER_DEFAULT_SCREEN_DPI * kMetersPerInch;
float screen_height_m =
    (float)screen_height_px / USER_DEFAULT_SCREEN_DPI * kMetersPerInch;

vec2 mouse_position;

float DisplayPxPerMeter() { return screen_width_px / screen_width_m; }
vec2 WindowSize() {
  return Vec2(window_width, window_height) / DisplayPxPerMeter();
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

std::unique_ptr<gui::Window> window;
std::unique_ptr<gui::Pointer> mouse;
std::unique_ptr<gui::Keyboard> keyboard;

gui::Pointer &GetMouse() {
  if (!mouse) {
    mouse =
        std::make_unique<gui::Pointer>(*window, ScreenToWindow(mouse_position));
    TRACKMOUSEEVENT track_mouse_event = {
        .cbSize = sizeof(TRACKMOUSEEVENT),
        .dwFlags = TME_LEAVE,
        .hwndTrack = main_window,
    };
    TrackMouseEvent(&track_mouse_event);
  }
  return *mouse;
}

void ResizeVulkan() {
  if (auto err = vk::Resize(window_width, window_height); !err.empty()) {
    MessageBox(nullptr, err.c_str(), "ALERT", 0);
  }
}

void Paint(SkCanvas &canvas) {
  if (!window) {
    return;
  }
  canvas.save();
  canvas.translate(0, window_height);
  canvas.scale(DisplayPxPerMeter(), -DisplayPxPerMeter());
  window->Draw(canvas);
  canvas.restore();
}

uint32_t ScanCode(LPARAM lParam) {
  uint32_t scancode = (lParam >> 16) & 0xff;
  bool extended = (lParam >> 24) & 0x1;
  if (extended) {
    if (scancode != 0x45) {
      scancode |= 0xE000;
    }
  } else {
    if (scancode == 0x45) {
      scancode = 0xE11D45;
    } else if (scancode == 0x54) {
      scancode = 0xE037;
    }
  }
  return scancode;
}

std::jthread render_thread;
time::point next_frame;
constexpr bool kPowersave = true;

#pragma comment(lib, "winmm.lib") // needed for timeBeginPeriod
#pragma comment(lib, "vk-bootstrap")

void VulkanPaint() {
  if (kPowersave) {
    time::point now = time::now();
    // TODO: Adjust next_frame to minimize input latency
    // VK_EXT_present_timing
    // https://github.com/KhronosGroup/Vulkan-Docs/pull/1364
    if (next_frame <= now) {
      double frame_count =
          ceil((now - next_frame).count() * screen_refresh_rate);
      next_frame += time::duration(frame_count / screen_refresh_rate);
      constexpr bool kLogSkippedFrames = false;
      if (kLogSkippedFrames && frame_count > 1) {
        LOG() << "Skipped " << (uint64_t)(frame_count - 1) << " frames";
      }
    } else {
      // This normally sleeps until T + ~10ms.
      // With timeBeginPeriod(1) it's T + ~1ms.
      // TODO: try condition_variable instead
      std::this_thread::sleep_until(next_frame);
      next_frame += time::duration(1.0 / screen_refresh_rate);
    }
  }
  SkCanvas *canvas = vk::GetBackbufferCanvas();
  if (anim) {
    anim.OnPaint(*canvas, Paint);
  } else {
    Paint(*canvas);
  }
  vk::Present();
}

void RenderThread(std::stop_token stop_token) {
  while (!stop_token.stop_requested()) {
    VulkanPaint();
  }
}

void RenderingStop() {
  if (render_thread.joinable()) {
    render_thread.request_stop();
    render_thread.join();
  }
}

void RenderingStart() {
  RenderingStop();
  render_thread = std::jthread(RenderThread);
}

void QueryDisplayCaps() {
  main_window_dpi = GetDpiForWindow(main_window);
  auto get_device_caps = [](HDC hdc) {
    screen_width_m = GetDeviceCaps(hdc, HORZSIZE) / 1000.0f;
    screen_height_m = GetDeviceCaps(hdc, VERTSIZE) / 1000.0f;
    screen_width_px = GetDeviceCaps(hdc, HORZRES);
    screen_height_px = GetDeviceCaps(hdc, VERTRES);
    screen_refresh_rate = GetDeviceCaps(hdc, VREFRESH);
    if (window) {
      window->DisplayPixelDensity(DisplayPxPerMeter());
    }
    constexpr bool kLogScreenCaps = true;
    if constexpr (kLogScreenCaps) {
      float diag = sqrt(screen_height_m * screen_height_m +
                        screen_width_m * screen_width_m) /
                   0.0254f;
      LOG() << "Display: " << f("%.1f", diag) << "' "
            << int(screen_width_m * 1000) << "x" << int(screen_height_m * 1000)
            << "mm (" << screen_width_px << "x" << screen_height_px << "px) "
            << screen_refresh_rate << "Hz";
    }
  };
  HMONITOR monitor = MonitorFromWindow(main_window, MONITOR_DEFAULTTONEAREST);
  MONITORINFOEX monitor_info = {};
  monitor_info.cbSize = sizeof(monitor_info);
  if (GetMonitorInfo(monitor, &monitor_info)) {
    HDC ic = CreateIC(nullptr, monitor_info.szDevice, nullptr, nullptr);
    if (ic) {
      get_device_caps(ic);
      DeleteDC(ic);
      return;
    }
  }
  HDC hdc = GetDC(main_window);
  get_device_caps(hdc);
  ReleaseDC(main_window, hdc);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  static unsigned char key_state[256] = {};
  static char utf8_buffer[4] = {};
  static int utf8_i = 0;
  if (std::optional<LRESULT> result =
          touchpad::ProcessEvent(uMsg, wParam, lParam)) {
    return *result;
  }
  switch (uMsg) {
  case WM_SIZE: {
    bool restart_rendering = render_thread.joinable();
    if (restart_rendering) {
      RenderingStop();
    }
    window_width = LOWORD(lParam);
    window_height = HIWORD(lParam);
    ResizeVulkan();
    if (window) {
      window->Resize(WindowSize());
    }
    VulkanPaint(); // for smoother resizing
    if (restart_rendering) {
      RenderingStart();
    }
    break;
  }
  case WM_MOVE: {
    client_x = LOWORD(lParam);
    client_y = HIWORD(lParam);
    break;
  }
  case WM_SETCURSOR: {
    // Intercept this message to prevent Windows from changing the cursor back
    // to an arrow.
    if (LOWORD(lParam) == HTCLIENT) {
      switch (GetMouse().Icon()) {
      case gui::Pointer::kIconArrow:
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
        break;
      case gui::Pointer::kIconHand:
        SetCursor(LoadCursor(nullptr, IDC_HAND));
        break;
      case gui::Pointer::kIconIBeam:
        SetCursor(LoadCursor(nullptr, IDC_IBEAM));
        break;
      }
      return TRUE;
    }
    return DefWindowProc(hWnd, uMsg, wParam, lParam);
  }
  case WM_PAINT: {
    ValidateRect(hWnd, nullptr);
    break;
  }
  case WM_DPICHANGED: {
    main_window_dpi = HIWORD(wParam);
    QueryDisplayCaps();
    RECT *const size_hint = (RECT *)lParam;
    SetWindowPos(hWnd, NULL, size_hint->left, size_hint->top,
                 size_hint->right - size_hint->left,
                 size_hint->bottom - size_hint->top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    break;
  }
  case WM_KEYDOWN: {
    auto scan_code = ScanCode(lParam);     // identifies the physical key
    uint8_t virtual_key = (uint8_t)wParam; // layout-dependent key code
    key_state[virtual_key] = 0x80;

    gui::Key key;
    key.physical = ScanCodeToKey(scan_code);
    key.logical = VirtualKeyToKey(virtual_key);

    char key_name[256];
    GetKeyNameText(lParam, key_name, sizeof(key_name));

    std::array<wchar_t, 16> utf16_buffer;
    int utf16_len = ToUnicode(virtual_key, scan_code, key_state,
                              utf16_buffer.data(), utf16_buffer.size(), 0);
    if (utf16_len) {
      std::array<char, 32> utf8_buffer = {};
      int utf8_len =
          SkUTF::UTF16ToUTF8(utf8_buffer.data(), utf8_buffer.size(),
                             (uint16_t *)utf16_buffer.data(), utf16_len);
      key.text = std::string(utf8_buffer.data(), utf8_len);
    }

    if (keyboard) {
      keyboard->KeyDown(key);
    }
    break;
  }
  case WM_KEYUP: {
    auto scan_code = ScanCode(lParam); // identifies the physical key
    uint8_t virtual_key = (uint8_t)wParam;
    key_state[virtual_key] = 0;
    // Right Alt sends WM_KEYDOWN for Control & Alt, but WM_KEYUP for Alt only.
    // This is a workaround for that.
    if (virtual_key == 0x12) {
      key_state[0x11] = 0;
    }

    gui::Key key;
    key.physical = ScanCodeToKey(scan_code);
    key.logical = VirtualKeyToKey(virtual_key);

    if (keyboard) {
      keyboard->KeyUp(key);
    }
    break;
  }
  case WM_CHAR: {
    uint8_t utf8_char = (uint8_t)wParam;
    uint32_t scan_code = ScanCode(lParam);
    utf8_buffer[utf8_i++] = utf8_char;
    bool utf8_complete = false;
    if ((utf8_buffer[0] & 0x80) == 0) {
      // 1-byte UTF-8 character
      utf8_complete = true;
    } else if ((utf8_buffer[0] & 0xE0) == 0xC0) {
      // 2-byte UTF-8 character
      utf8_complete = (utf8_i == 2);
    } else if ((utf8_buffer[0] & 0xF0) == 0xE0) {
      // 3-byte UTF-8 character
      utf8_complete = (utf8_i == 3);
    } else if ((utf8_buffer[0] & 0xF8) == 0xF0) {
      // 4-byte UTF-8 character
      utf8_complete = (utf8_i == 4);
    } else {
      // Invalid UTF-8 character
      ERROR() << "Invalid UTF-8 start byte: 0x" << f("%x", utf8_char) << " ("
              << utf8_char << "), scancode=0x" << f("%x", scan_code);
      utf8_i = 0;
    }
    if (utf8_complete) {
      if (keyboard) {
        gui::Key key;
        key.physical = ScanCodeToKey(scan_code);
        key.logical = gui::AnsiKey::Unknown;
        key.text = std::string(utf8_buffer, utf8_i);
        keyboard->KeyDown(key);
        keyboard->KeyUp(key);
      }
      utf8_i = 0;
    }
    break;
  }
  case WM_LBUTTONDOWN: {
    GetMouse().ButtonDown(gui::kMouseLeft);
    break;
  }
  case WM_LBUTTONUP: {
    GetMouse().ButtonUp(gui::kMouseLeft);
    break;
  }
  case WM_MBUTTONDOWN: {
    GetMouse().ButtonDown(gui::kMouseMiddle);
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
  case WM_MOUSELEAVE: {
    mouse.reset();
    break;
  }
  case WM_MOUSEWHEEL: {
    int16_t x = lParam & 0xFFFF;
    int16_t y = (lParam >> 16) & 0xFFFF;
    int16_t delta = GET_WHEEL_DELTA_WPARAM(wParam);
    int16_t keys = GET_KEYSTATE_WPARAM(wParam);
    if (!touchpad::ShouldIgnoreScrollEvents()) {
      GetMouse().Wheel(delta / 120.0);
    }
    break;
  }
  case WM_VSCROLL: {
    LOG() << "WM_VSCROLL";
    break;
  }
  case WM_HSCROLL: {
    LOG() << "WM_HSCROLL";
    break;
  }
  case WM_GESTURE: {
    LOG() << "WM_GESTURE";
    break;
  }
  case WM_DESTROY: {
    PostQuitMessage(0);
    break;
  }
  default:
    return DefWindowProc(hWnd, uMsg, wParam, lParam);
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
  // This makes std::this_thread::sleep_until() more accurate.
  timeBeginPeriod(1);

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
  touchpad::Init();

  QueryDisplayCaps();

  if (auto err = vk::Init(); !err.empty()) {
    FATAL() << "Failed to initialize Vulkan: " << err;
  }

  next_frame = time::now();
  ShowWindow(main_window, nCmdShow);
  UpdateWindow(main_window);
  RECT rect;
  GetClientRect(main_window, &rect);
  window_width = rect.right - rect.left;
  window_height = rect.bottom - rect.top;
  window.reset(new gui::Window(WindowSize(), DisplayPxPerMeter()));
  keyboard = std::make_unique<gui::Keyboard>(*window);
  ResizeVulkan();
  RenderingStart();

  while (true) {
    MSG msg = {};
    switch (GetMessage(&msg, nullptr, 0, 0)) {
    case -1: // error
      ERROR() << "GetMessage failed: " << GetLastError();
    case 0: // fallthrough to WM_QUIT
      RenderingStop();
      vk::Destroy();
      window.reset(); // delete it manually because destruction from the fini
                      // section happens after global destructors (specifically
                      // the `windows` list).
      return (int)msg.wParam;
    default:
      // For some reason TranslateMessage generates CP-1250 characters instead
      // of UTF-8. TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
  }
  __builtin_unreachable();
}
