#undef NOGDI
#include <windows.h>
#undef ERROR

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shlwapi.lib")

#include <include/core/SkCanvas.h>
#include <include/core/SkGraphics.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/rapidjson.h>
#include <shlwapi.h>
#include <src/base/SkUTF.h>
#include <timeapi.h>
#include <winuser.h>

#include <memory>
#include <thread>

#include "automat.hh"
#include "backtrace.hh"
#include "hid.hh"
#include "library.hh"  // IWYU pragma: export
#include "loading_animation.hh"
#include "persistence.hh"
#include "root.hh"
#include "thread_name.hh"
#include "touchpad.hh"
#include "vk.hh"
#include "win.hh"
#include "win_key.hh"
#include "win_main.hh"
#include "window.hh"

using namespace automat;
using namespace automat::gui;
using namespace maf;

HWND main_window;

int client_x;
int client_y;
int client_width;
int client_height;

// Placeholder values for screen size. They should be updated when window is
// resized.
int screen_left_px = 0;
int screen_top_px = 0;
int screen_width_px = 1920;
int screen_height_px = 1080;
int screen_refresh_rate = 60;
float screen_width_m = (float)screen_width_px / USER_DEFAULT_SCREEN_DPI * kMetersPerInch;
float screen_height_m = (float)screen_height_px / USER_DEFAULT_SCREEN_DPI * kMetersPerInch;

Vec2 mouse_position;

float DisplayPxPerMeter() { return screen_width_px / screen_width_m; }
Vec2 WindowSize() { return Vec2(client_width, client_height) / DisplayPxPerMeter(); }

// Coordinate spaces:
// - Screen (used by Windows & win_main.cc, origin in the top left corner of the
// screen, Y axis goes down)
// - Window (used for win_main.cc <=> gui.cc communication, origin in the bottom
// left corner of the window, Y axis goes up)
// - Canvas (used for gui.cc <=> Automat communication, origin in the center
// of the window, Y axis goes up)

namespace automat::gui {

Vec2 ScreenToWindow(Vec2 screen) {
  Vec2 window = (screen - Vec2(client_x, client_y + client_height)) / DisplayPxPerMeter();
  window.y = -window.y;
  return window;
}

Vec2 WindowToScreen(Vec2 window) {
  window.y = -window.y;
  return window * DisplayPxPerMeter() + Vec2(client_x, client_height + client_y);
}

Vec2 GetMainPointerScreenPos() { return mouse_position; }

}  // namespace automat::gui

std::unique_ptr<gui::Pointer> mouse;

gui::Pointer& GetMouse() {
  if (!mouse) {
    mouse = std::make_unique<gui::Pointer>(*window, automat::gui::ScreenToWindow(mouse_position));
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
  if (auto err = vk::Resize(client_width, client_height); !err.empty()) {
    MessageBox(nullptr, err.c_str(), "ALERT", 0);
  }
}

void Paint(SkCanvas& canvas) {
  if (!window) {
    return;
  }
  canvas.save();
  canvas.scale(DisplayPxPerMeter(), DisplayPxPerMeter());
  window->Draw(canvas);
  canvas.restore();
}

uint32_t ScanCode(LPARAM lParam) {
  uint32_t scancode = (lParam >> 16) & 0xff;
  bool extended = (lParam >> 24) & 0x1;
  if (extended) {
    if (scancode == 0x45) {  // Pause button
      // scancode = 0xE11D45;
    } else {
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
time::SystemPoint next_frame;
constexpr bool kPowersave = true;

#pragma comment(lib, "winmm.lib")  // needed for timeBeginPeriod
#pragma comment(lib, "vk-bootstrap")

void VulkanPaint() {
  if (!vk::initialized) {
    return;
  }
  if (kPowersave) {
    time::SystemPoint now = time::SystemNow();
    // TODO: Adjust next_frame to minimize input latency
    // VK_EXT_present_timing
    // https://github.com/KhronosGroup/Vulkan-Docs/pull/1364
    if (next_frame <= now) {
      double frame_count = ceil((now - next_frame).count() * screen_refresh_rate);
      next_frame += time::Duration(frame_count / screen_refresh_rate);
      constexpr bool kLogSkippedFrames = false;
      if (kLogSkippedFrames && frame_count > 1) {
        LOG << "Skipped " << (uint64_t)(frame_count - 1) << " frames";
      }
    } else {
      // This normally sleeps until T + ~10ms.
      // With timeBeginPeriod(1) it's T + ~1ms.
      // TODO: try condition_variable instead
      std::this_thread::sleep_until(next_frame);
      next_frame += time::Duration(1.0 / screen_refresh_rate);
    }
  }
  SkCanvas* canvas = vk::GetBackbufferCanvas();
  if (anim) {
    anim.OnPaint(*canvas, Paint);
  } else {
    Paint(*canvas);
  }
  vk::Present();
}

void RenderThread(std::stop_token stop_token) {
  SetThreadName("Render Thread");
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
  constexpr bool kLogScreenCaps = false;
  {
    screen_left_px = GetSystemMetrics(SM_XVIRTUALSCREEN);
    screen_width_px = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    screen_height_px = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    screen_top_px = GetSystemMetrics(SM_YVIRTUALSCREEN);
    if constexpr (kLogScreenCaps) {
      LOG << "Virtual screen: left=" << screen_left_px << ", top=" << screen_top_px << ", "
          << screen_width_px << "x" << screen_height_px;
    }
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR hMonitor, HDC crappy_hdc, LPRECT rect, LPARAM) {
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
          screen_width_m = screen_width_px * monitor_diagonal_m / monitor_diagonal_px;
          screen_height_m = screen_height_px * monitor_diagonal_m / monitor_diagonal_px;
          screen_refresh_rate = GetDeviceCaps(hdc, VREFRESH);
          DeleteDC(hdc);

          if (window) {
            window->DisplayPixelDensity(DisplayPxPerMeter());
          }

          if constexpr (kLogScreenCaps) {
            float diag =
                sqrt(screen_height_m * screen_height_m + screen_width_m * screen_width_m) / 0.0254f;
            LOG << "Display: " << f("%.1f", diag) << "″ " << int(screen_width_m * 1000) << "x"
                << int(screen_height_m * 1000) << "mm (" << screen_width_px << "x"
                << screen_height_px << "px) " << screen_refresh_rate << "Hz";
          }

          return TRUE;
        },
        0);
  }
}

bool IsMaximized(HWND hWnd) {
  WINDOWPLACEMENT placement = {};
  placement.length = sizeof(WINDOWPLACEMENT);
  GetWindowPlacement(hWnd, &placement);
  return placement.showCmd == SW_SHOWMAXIMIZED;
}

bool window_active = false;
bool keylogging_enabled = false;

void RegisterRawInput(bool keylogging) {
  vector<RAWINPUTDEVICE> rids;
  rids.emplace_back(touchpad::GetRAWINPUTDEVICE());
  rids.emplace_back(RAWINPUTDEVICE{
      .usUsagePage = hid::UsagePage_GenericDesktop,
      .usUsage = hid::Usage_GenericDesktop_Keyboard,
      .dwFlags =
          (keylogging ? RIDEV_INPUTSINK
                      : (DWORD)0) |  // captures input even when the window is not in the foreground
          RIDEV_NOLEGACY,            // adds keyboard and also ignores legacy keyboard messages
      .hwndTarget = main_window,
  });
  BOOL register_result = RegisterRawInputDevices(rids.data(), rids.size(), sizeof(RAWINPUTDEVICE));
  if (!register_result) {
    ERROR << "Failed to register raw input device";
  }
  keylogging_enabled = keylogging;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  static unsigned char key_state[256] = {};
  auto IsCtrlDown = []() {
    return key_state[VK_LCONTROL] || key_state[VK_RCONTROL] || key_state[VK_CONTROL];
  };
  static char utf8_buffer[4] = {};
  static int utf8_i = 0;
  if (std::optional<LRESULT> result = touchpad::ProcessEvent(uMsg, wParam, lParam)) {
    return *result;
  }
  switch (uMsg) {
    case WM_SIZE: {
      bool restart_rendering = render_thread.joinable();
      if (restart_rendering) {
        RenderingStop();
      }
      client_width = LOWORD(lParam);
      client_height = HIWORD(lParam);
      ResizeVulkan();
      if (window) {
        window->Resize(WindowSize());
        bool is_maximized = wParam == SIZE_MAXIMIZED;
        window->maximized_horizontally = is_maximized;
        window->maximized_vertically = is_maximized;
      }
      VulkanPaint();  // for smoother resizing
      if (restart_rendering) {
        RenderingStart();
      }
      break;
    }
    case WM_MOVE: {
      client_x = LOWORD(lParam);
      client_y = HIWORD(lParam);
      if (window) {
        float left = std::max<float>(client_x / DisplayPxPerMeter(), 0.f);
        float right = std::min<float>(
            (client_x + client_width - screen_width_px) / DisplayPxPerMeter(), -0.f);
        if (left < fabsf(right)) {
          window->output_device_x = left;
        } else {
          window->output_device_x = right;
        }
        float top = std::max<float>(client_y / DisplayPxPerMeter(), 0.f);
        float bottom = std::min<float>(
            (client_y + client_height - screen_height_px) / DisplayPxPerMeter(), -0.f);
        if (top < fabsf(bottom)) {
          window->output_device_y = top;
        } else {
          window->output_device_y = bottom;
        }
      }
      break;
    }
    case WM_SETCURSOR: {
      // Intercept this message to prevent Windows from changing the cursor back
      // to an arrow.
      if (LOWORD(lParam) == HTCLIENT) {
        gui::Pointer::IconType icon;
        RunOnAutomatThreadSynchronous([&]() { icon = GetMouse().Icon(); });
        switch (icon) {
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
      QueryDisplayCaps();
      RECT* const size_hint = (RECT*)lParam;
      SetWindowPos(hWnd, NULL, size_hint->left, size_hint->top, size_hint->right - size_hint->left,
                   size_hint->bottom - size_hint->top, SWP_NOZORDER | SWP_NOACTIVATE);
      break;
    }
    case WM_ACTIVATEAPP: {
      window_active = wParam != WA_INACTIVE;
      return DefWindowProc(hWnd, uMsg, wParam, lParam);
    }
    case WM_INPUT: {
      HRAWINPUT hRawInput = (HRAWINPUT)lParam;
      UINT size = 0;
      UINT ret = GetRawInputData(hRawInput, RID_INPUT, NULL, &size, sizeof(RAWINPUTHEADER));
      if (ret == (UINT)-1) {  // error when retrieving size of buffer
        ERROR << "Error when retrieving size of buffer. Error code: " << GetLastError();
        return DefWindowProc(main_window, uMsg, wParam, lParam);
      }
      alignas(8) uint8_t raw_input_buffer[size];
      RAWINPUT* raw_input = (RAWINPUT*)raw_input_buffer;
      UINT size_copied =
          GetRawInputData(hRawInput, RID_INPUT, raw_input_buffer, &size, sizeof(RAWINPUTHEADER));
      if (size != size_copied) {  // error when retrieving buffer
        ERROR << "Error when retrieving buffer. Size=" << size << " Error code: " << GetLastError();
        return DefWindowProc(main_window, uMsg, wParam, lParam);
      }
      if (raw_input->header.dwType != RIM_TYPEKEYBOARD) {
        ERROR << "Received WM_INPUT event with type other than RIM_TYPEKEYBOARD";
        return DefWindowProc(main_window, uMsg, wParam, lParam);
      }
      RAWKEYBOARD& ev = raw_input->data.keyboard;
      U32 scan_code = ev.MakeCode;
      if (ev.Flags & RI_KEY_E0) {
        scan_code |= 0xE000;
      }
      if (ev.Flags & RI_KEY_E1) {
        scan_code |= 0xE11D00;
      }
      auto physical = ScanCodeToKey(scan_code);
      auto virtual_key = KeyToVirtualKey(physical);  // layout-dependent key code

      gui::Key key;
      key.physical = physical;
      key.logical = VirtualKeyToKey(virtual_key);

      if (ev.Message == WM_KEYDOWN) {
        if (key_state[virtual_key] == 0) {
          key_state[virtual_key] = 0x80;
          key.ctrl = IsCtrlDown();
          std::array<wchar_t, 16> utf16_buffer;
          int utf16_len = ToUnicode(virtual_key, scan_code, key_state, utf16_buffer.data(),
                                    utf16_buffer.size(), 0);
          if (utf16_len) {
            std::array<char, 32> utf8_buffer = {};
            int utf8_len = SkUTF::UTF16ToUTF8(utf8_buffer.data(), utf8_buffer.size(),
                                              (uint16_t*)utf16_buffer.data(), utf16_len);
            key.text = std::string(utf8_buffer.data(), utf8_len);
          }
          if (gui::keyboard) {
            if (keylogging_enabled) {
              gui::keyboard->LogKeyDown(key);
            }
            if (window_active) {
              gui::keyboard->KeyDown(key);
            }
          }
        }
      } else if (ev.Message == WM_KEYUP) {
        key_state[virtual_key] = 0;
        // Right Alt sends WM_KEYDOWN for Control & Alt, but WM_KEYUP for Alt only.
        // This is a workaround for that.
        if (virtual_key == 0x12) {
          key_state[0x11] = 0;
        }
        key.ctrl = IsCtrlDown();
        if (gui::keyboard) {
          if (keylogging_enabled) {
            gui::keyboard->LogKeyUp(key);
          }
          if (window_active) {
            gui::keyboard->KeyUp(key);
          }
        }
      } else if (ev.Message == WM_SYSKEYDOWN) {
        // ignore
      } else {
        LOG << "Unknown WM_INPUT Message: " << dump_struct(ev);
      }
      return DefWindowProc(main_window, uMsg, wParam, lParam);
    }
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_CHAR:
      break;
    case WM_HOTKEY: {
      int id = wParam;  // discard the upper 32 bits
      // lParam carries the modifiers (first 16 bits) and then the keycode
      RunOnAutomatThread([id]() { automat::gui::OnHotKeyDown(id); });
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
      mouse_position.x = x + client_x;
      mouse_position.y = y + client_y;
      GetMouse().Move(automat::gui::ScreenToWindow(mouse_position));
      break;
    }
    case WM_MOUSELEAVE: {
      RunOnAutomatThread([]() { mouse.reset(); });
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
      LOG << "WM_VSCROLL";
      break;
    }
    case WM_HSCROLL: {
      LOG << "WM_HSCROLL";
      break;
    }
    case WM_GESTURE: {
      LOG << "WM_GESTURE";
      break;
    }
    case WM_USER: {
      std::function<void()>* f = (std::function<void()>*)lParam;
      (*f)();
      delete f;
      break;
    }
    case WM_CLOSE: {
      RenderingStop();
      StopRoot();
      Status status;
      SaveState(*window, status);
      if (!OK(status)) {
        ERROR << "Couldn't save automat state: " << status;
      }
      DestroyWindow(hWnd);
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

std::thread::id windows_thread_id;

void RunOnWindowsThread(std::function<void()>&& f) {
  if (std::this_thread::get_id() == windows_thread_id) {
    f();
    return;
  }
  PostMessage(main_window, WM_USER, 0, (LPARAM) new std::function<void()>(std::move(f)));
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR pCmdLine, int nCmdShow) {
  EnableBacktraceOnSIGSEGV();
  SetThreadName("WinMain");
  windows_thread_id = std::this_thread::get_id();

  // Switch to UTF-8
  setlocale(LC_CTYPE, ".utf8");
  SetConsoleCP(CP_UTF8);
  SetConsoleOutputCP(CP_UTF8);
  // This makes std::this_thread::sleep_until() more accurate.
  timeBeginPeriod(1);

  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  SkGraphics::Init();

  if (!RegisterClassEx(&GetWindowClass())) {
    FATAL << "Failed to register window class.";
  }

  Status status;
  InitAutomat(status);
  if (!OK(status)) {
    ERROR << "Failed to load Automat state: " << status;
    // Try to continue with damaged state.
  }
  anim.LoadingCompleted();

  QueryDisplayCaps();

  // Save the window size and position - those values may be overwritten by WndProc when the window
  // is created.
  auto desired_size = window->size;
  Vec2 desired_pos = Vec2(window->output_device_x, window->output_device_y);
  bool maximized = window->maximized_horizontally || window->maximized_vertically;
  main_window =
      CreateWindowEx(WS_EX_OVERLAPPEDWINDOW, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW, 0, 0,
                     CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr, GetInstance(), nullptr);
  if (!main_window) {
    FATAL << "Failed to create main window.";
  }

  window->RequestResize = [&](Vec2 new_size) {
    int w = roundf(new_size.x * DisplayPxPerMeter());
    int h = roundf(new_size.y * DisplayPxPerMeter());

    // If the window is maximized and requested size is different, un-maximize it first.
    if (w == client_width && h == client_height) {
      return;
    }
    if (IsMaximized(main_window)) {
      ShowWindow(main_window, SW_RESTORE);
    }

    // Account for window border when calling SetWindowPos
    RECT client_rect, window_rect;
    GetClientRect(main_window, &client_rect);
    GetWindowRect(main_window, &window_rect);
    float vertical_frame_adjustment = (window_rect.bottom - window_rect.top) - client_rect.bottom;
    float horizontal_frame_adjustment = (window_rect.right - window_rect.left) - client_rect.right;
    h += vertical_frame_adjustment;
    w += horizontal_frame_adjustment;
    SetWindowPos(main_window, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER);
  };
  window->RequestMaximize = [&](bool horiz, bool vert) {
    if (horiz || vert) {
      ShowWindow(main_window, SW_MAXIMIZE);
    }
  };

  // Restore the position of the client area.
  if (!maximized && !isnan(desired_pos.x) && !isnan(desired_pos.y)) {
    int client_x, client_y;
    if (signbit(desired_pos.x)) {  // desired_pos.x is negative - distance from the right edge
      client_x = screen_left_px +
                 roundf(screen_width_px + (desired_pos.x - desired_size.x) * DisplayPxPerMeter());
    } else {
      client_x = screen_left_px + roundf(desired_pos.x * DisplayPxPerMeter());
    }
    if (signbit(desired_pos.y)) {  // desired_pos.y is negative - distance from the bottom edge
      client_y = screen_top_px +
                 roundf(screen_height_px + (desired_pos.y - desired_size.y) * DisplayPxPerMeter());
    } else {
      client_y = screen_top_px + roundf(desired_pos.y * DisplayPxPerMeter());
    }
    POINT zero = {0, 0};
    ClientToScreen(main_window, &zero);
    int delta_x = client_x - zero.x;
    int delta_y = client_y - zero.y;
    SetWindowPos(main_window, nullptr, delta_x, delta_y, 0, 0, SWP_NOZORDER | SWP_NOSIZE);
  }

  if (!maximized) {
    window->RequestResize(desired_size);
  }

  RegisterRawInput();

  if (auto err = vk::Init(); !err.empty()) {
    FATAL << "Failed to initialize Vulkan: " << err;
  }
  ResizeVulkan();

  next_frame = time::SystemNow();
  RenderingStart();

  ShowWindow(main_window, maximized ? SW_SHOWMAXIMIZED : nCmdShow);
  UpdateWindow(main_window);

  while (true) {
    MSG msg = {};
    switch (GetMessage(&msg, nullptr, 0, 0)) {
      case -1:  // error
        ERROR << "GetMessage failed: " << GetLastError();
      case 0:  // fallthrough to WM_QUIT
        RenderingStop();
        vk::Destroy();
        window.reset();  // delete it manually because destruction from the fini
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
