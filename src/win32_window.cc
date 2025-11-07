// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "win32_window.hh"

#include <src/base/SkUTF.h>
#include <winuser.h>

#include "automat.hh"
#include "hid.hh"
#include "key.hh"
#include "log.hh"
#include "optional.hh"
#include "root_widget.hh"
#include "status.hh"
#include "touchpad.hh"
#include "widget.hh"
#include "win32.hh"
#include "win_key.hh"

using namespace automat;
using namespace automat::ui;
using namespace automat::win32;
using namespace std;

map<HWND, Win32Window*> hwnd_to_window;

// Forward declaration for the struct
struct Win32PointerGrab;

// Forward declaration for the hook procedure
static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam);

// Global hook handle and pointer grab reference
static HHOOK global_mouse_hook = nullptr;
static std::vector<Win32PointerGrab*> active_pointer_grabs;

struct Win32PointerGrab : automat::ui::PointerGrab {
  Win32Window& win32_window;
  Optional<automat::ui::Pointer::IconOverride> crosshair_icon;

  Win32PointerGrab(automat::ui::Pointer& pointer, automat::ui::PointerGrabber& grabber,
                   Win32Window& win32_window)
      : automat::ui::PointerGrab(pointer, grabber), win32_window(win32_window) {
    if (active_pointer_grabs.size() == 0) {
      global_mouse_hook =
          SetWindowsHookExA(WH_MOUSE_LL, LowLevelMouseProc, GetModuleHandle(nullptr), 0);
      if (!global_mouse_hook) {
        ERROR << "Failed to install global mouse hook: " << GetLastError();
        return;
      }
      crosshair_icon.emplace(pointer, ui::Pointer::kIconCrosshair);
    }

    if (global_mouse_hook) {
      active_pointer_grabs.push_back(this);
    }
  }

  ~Win32PointerGrab() {
    if (auto it = std::find(active_pointer_grabs.begin(), active_pointer_grabs.end(), this);
        it != active_pointer_grabs.end()) {
      active_pointer_grabs.erase(it);
    }
    if (active_pointer_grabs.size() == 0 && global_mouse_hook) {
      UnhookWindowsHookEx(global_mouse_hook);
      global_mouse_hook = nullptr;
      crosshair_icon.reset();
    }
  }
};

struct Win32Pointer : automat::ui::Pointer {
  Win32Window& win32_window;

  Win32Pointer(automat::ui::RootWidget& root, Vec2 position, Win32Window& win32_window)
      : automat::ui::Pointer(root, position), win32_window(win32_window) {}

  void OnIconChanged(automat::ui::Pointer::IconType old_icon,
                     automat::ui::Pointer::IconType new_icon) override {
    UpdateCursor(new_icon);
  }

  void UpdateCursor(automat::ui::Pointer::IconType icon) {
    HCURSOR cursor;
    switch (icon) {
      case automat::ui::Pointer::kIconArrow:
        cursor = LoadCursor(nullptr, IDC_ARROW);
        break;
      case automat::ui::Pointer::kIconHand:
        cursor = LoadCursor(nullptr, IDC_HAND);
        break;
      case automat::ui::Pointer::kIconIBeam:
        cursor = LoadCursor(nullptr, IDC_IBEAM);
        break;
      case automat::ui::Pointer::kIconAllScroll:
        cursor = LoadCursor(nullptr, IDC_SIZEALL);
        break;
      case automat::ui::Pointer::kIconResizeHorizontal:
        cursor = LoadCursor(nullptr, IDC_SIZEWE);
        break;
      case automat::ui::Pointer::kIconResizeVertical:
        cursor = LoadCursor(nullptr, IDC_SIZENS);
        break;
      case automat::ui::Pointer::kIconCrosshair:
        cursor = LoadCursor(nullptr, IDC_CROSS);
        break;
      default:
        cursor = LoadCursor(nullptr, IDC_ARROW);
        break;
    }
    SetCursor(cursor);
  }

  automat::ui::PointerGrab& RequestGlobalGrab(automat::ui::PointerGrabber& grabber) override {
    grab.reset(new Win32PointerGrab(*this, grabber, win32_window));
    return *grab;
  }
};

void SetCursorTimer(HWND hwnd, UINT message_wm_timer, UINT_PTR timer_id, DWORD time) {
  SendMessageA(hwnd, WM_SETCURSOR, 0, HTCLIENT);
  KillTimer(hwnd, timer_id);
}

// Low-level mouse hook procedure
static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
  if (nCode >= 0) {
    MSLLHOOKSTRUCT* mouse_struct = (MSLLHOOKSTRUCT*)lParam;
    // Handle different mouse messages
    switch (wParam) {
      case WM_MOUSEMOVE: {
        SetCursorPos(mouse_struct->pt.x, mouse_struct->pt.y);
        for (int i = active_pointer_grabs.size() - 1; i >= 0; --i) {
          auto* grab = active_pointer_grabs[i];
          auto& window = grab->win32_window;
          auto lock = window.Lock();
          window.mouse_position.x = mouse_struct->pt.x;
          window.mouse_position.y = mouse_struct->pt.y;
          grab->pointer.Move(window.ScreenToWindowPx(window.mouse_position));
          if (i == 0) {
            auto hwnd = grab->win32_window.hwnd;
            // Change the cursor immediately
            static_cast<Win32Pointer&>(grab->pointer).UpdateCursor(grab->pointer.Icon());
            // First timer helps restoring the cursor when the OS changes the cursor back to arrow
            // in the first 10ms
            SetTimer(hwnd, 1, 10, SetCursorTimer);
            // Sometimes the OS is late to change the cursor back to arrow, so we set a second timer
            SetTimer(hwnd, 2, 50, SetCursorTimer);
          }
        }
        break;
      }
      case WM_LBUTTONDOWN: {
        for (int i = active_pointer_grabs.size() - 1; i >= 0; --i) {
          auto* grab = active_pointer_grabs[i];
          grab->pointer.ButtonDown(ui::PointerButton::Left);
        }
        break;
      }
      case WM_LBUTTONUP: {
        for (int i = active_pointer_grabs.size() - 1; i >= 0; --i) {
          auto* grab = active_pointer_grabs[i];
          grab->pointer.ButtonUp(ui::PointerButton::Left);
        }
        break;
      }
      case WM_RBUTTONDOWN: {
        for (int i = active_pointer_grabs.size() - 1; i >= 0; --i) {
          auto* grab = active_pointer_grabs[i];
          grab->pointer.ButtonDown(ui::PointerButton::Right);
        }
        break;
      }
      case WM_RBUTTONUP: {
        for (int i = active_pointer_grabs.size() - 1; i >= 0; --i) {
          auto* grab = active_pointer_grabs[i];
          grab->pointer.ButtonUp(ui::PointerButton::Right);
        }
        break;
      }
      case WM_MBUTTONDOWN: {
        for (int i = active_pointer_grabs.size() - 1; i >= 0; --i) {
          auto* grab = active_pointer_grabs[i];
          grab->pointer.ButtonDown(ui::PointerButton::Middle);
        }
        break;
      }
      case WM_MBUTTONUP: {
        for (int i = active_pointer_grabs.size() - 1; i >= 0; --i) {
          auto* grab = active_pointer_grabs[i];
          grab->pointer.ButtonUp(ui::PointerButton::Middle);
        }
        break;
      }
      case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(mouse_struct->mouseData);
        for (int i = active_pointer_grabs.size() - 1; i >= 0; --i) {
          auto* grab = active_pointer_grabs[i];
          grab->pointer.Wheel(delta / 120.0f);
        }
        break;
      }
      case WM_XBUTTONDOWN: {
        int button = GET_XBUTTON_WPARAM(mouse_struct->mouseData);
        for (int i = active_pointer_grabs.size() - 1; i >= 0; --i) {
          auto* grab = active_pointer_grabs[i];
          grab->pointer.ButtonDown(button == XBUTTON1 ? ui::PointerButton::Back
                                                      : ui::PointerButton::Forward);
        }
        break;
      }
      case WM_XBUTTONUP: {
        int button = GET_XBUTTON_WPARAM(mouse_struct->mouseData);
        for (int i = active_pointer_grabs.size() - 1; i >= 0; --i) {
          auto* grab = active_pointer_grabs[i];
          grab->pointer.ButtonUp(button == XBUTTON1 ? ui::PointerButton::Back
                                                    : ui::PointerButton::Forward);
        }
        break;
      }
    }

    // Prevent the event from reaching other applications
    return 1;
  }

  // Call next hook
  return CallNextHookEx(global_mouse_hook, nCode, wParam, lParam);
}

Win32Window::Win32Window(automat::ui::RootWidget& root) : automat::ui::Window(root) {}

Win32Window::~Win32Window() {
  if (hwnd) {
    hwnd_to_window.erase(hwnd);
    hwnd = nullptr;
  }
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  auto window_it = hwnd_to_window.find(hWnd);
  if (window_it == hwnd_to_window.end()) {
    return DefWindowProc(hWnd, uMsg, wParam, lParam);
  }
  Win32Window& window = *window_it->second;

  // TODO: Move this into Win32Window
  static unsigned char key_state[256] = {};
  auto IsCtrlDown = []() {
    return key_state[VK_LCONTROL] || key_state[VK_RCONTROL] || key_state[VK_CONTROL];
  };
  static char utf8_buffer[4] = {};
  static int utf8_i = 0;
  if (std::optional<LRESULT> result = touchpad::ProcessEvent(hWnd, uMsg, wParam, lParam)) {
    return *result;
  }
  switch (uMsg) {
    case WM_SIZE: {
      auto lock = window.Lock();
      window.client_width = LOWORD(lParam);
      window.client_height = HIWORD(lParam);
      if (root_widget) {
        auto size_px = Vec2(window.client_width, window.client_height);
        auto size_m = size_px / win32::caps.px_per_meter;
        root_widget->Resized(size_m);
        bool is_maximized = wParam == SIZE_MAXIMIZED;
        root_widget->Maximized(is_maximized, is_maximized);
      }
      break;
    }
    case WM_MOVE: {
      auto lock = window.Lock();
      window.client_x = LOWORD(lParam);
      window.client_y = HIWORD(lParam);
      if (root_widget) {
        float left = std::max<float>(window.client_x / win32::caps.px_per_meter, 0.f);
        float right =
            std::min<float>((window.client_x + window.client_width - win32::caps.screen_width_px) /
                                win32::caps.px_per_meter,
                            -0.f);
        if (left < fabsf(right)) {
          root_widget->output_device_x = left;
        } else {
          root_widget->output_device_x = right;
        }
        float top = std::max<float>(window.client_y / win32::caps.px_per_meter, 0.f);
        float bottom = std::min<float>(
            (window.client_y + window.client_height - win32::caps.screen_height_px) /
                win32::caps.px_per_meter,
            -0.f);
        if (top < fabsf(bottom)) {
          root_widget->output_device_y = top;
        } else {
          root_widget->output_device_y = bottom;
        }
      }
      break;
    }
    case WM_SETCURSOR: {
      // Intercept this message to prevent Windows from changing the cursor back
      // to an arrow.
      if (LOWORD(lParam) == HTCLIENT) {
        ui::Pointer::IconType icon;
        {
          auto lock = window.Lock();
          auto& mouse = static_cast<Win32Pointer&>(window.GetMouse());
          icon = mouse.Icon();
          mouse.UpdateCursor(icon);
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
      auto lock = window.Lock();
      win32::caps = win32::DisplayCaps::Query();
      window.root.DisplayPixelDensity(win32::caps.px_per_meter);
      RECT* const size_hint = (RECT*)lParam;
      SetWindowPos(hWnd, NULL, size_hint->left, size_hint->top, size_hint->right - size_hint->left,
                   size_hint->bottom - size_hint->top, SWP_NOZORDER | SWP_NOACTIVATE);
      break;
    }
    case WM_ACTIVATEAPP: {
      auto lock = window.Lock();
      window.window_active = wParam != WA_INACTIVE;
      return DefWindowProc(hWnd, uMsg, wParam, lParam);
    }
    case WM_INPUT: {
      HRAWINPUT hRawInput = (HRAWINPUT)lParam;
      UINT size = 0;
      UINT ret = GetRawInputData(hRawInput, RID_INPUT, NULL, &size, sizeof(RAWINPUTHEADER));
      if (ret == (UINT)-1) {  // error when retrieving size of buffer
        ERROR << "Error when retrieving size of buffer. Error code: " << GetLastError();
        return DefWindowProc(hWnd, uMsg, wParam, lParam);
      }
      alignas(8) uint8_t raw_input_buffer[size];
      RAWINPUT* raw_input = (RAWINPUT*)raw_input_buffer;
      UINT size_copied =
          GetRawInputData(hRawInput, RID_INPUT, raw_input_buffer, &size, sizeof(RAWINPUTHEADER));
      if (size != size_copied) {  // error when retrieving buffer
        ERROR << "Error when retrieving buffer. Size=" << size << " Error code: " << GetLastError();
        return DefWindowProc(hWnd, uMsg, wParam, lParam);
      }
      if (raw_input->header.dwType == RIM_TYPEKEYBOARD) {
        // Key Mapping:
        // Ignore the VKey provided by the OS.
        // Take the MakeCode, append the E0/E11 prefix (if present) and translate to physical key
        // locally.
        // Also ignore the Message field and instead check for the RI_KEY_BREAK to detect if key is
        // down or up.
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

        ui::Key key;
        key.physical = physical;
        key.logical = VirtualKeyToKey(virtual_key);

        if (key.logical == AnsiKey::AltRight && ev.VKey == VK_CONTROL) {
          // Right Alt sends key double events for VKey set to (first) Control & (second) Alt.
          // We ignore the ones with VKey set to VK_CONTROL.
          return DefWindowProc(hWnd, uMsg, wParam, lParam);
        }

        bool down = !(ev.Flags & RI_KEY_BREAK);

        if (down) {
          // LOG << "Pressed " << ui::ToStr(key.logical);
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
            auto lock = window.Lock();
            if (window.keylogging_enabled) {
              window.root.keyboard.LogKeyDown(key);
            }
            if (window.window_active) {
              window.root.keyboard.KeyDown(key);
            }
          }
        } else {
          // LOG << "Released " << ui::ToStr(key.logical);
          key_state[virtual_key] = 0;
          key.ctrl = IsCtrlDown();
          auto lock = window.Lock();
          if (window.keylogging_enabled) {
            window.root.keyboard.LogKeyUp(key);
          }
          if (window.window_active) {
            window.root.keyboard.KeyUp(key);
          }
        }
      } else if (raw_input->header.dwType == RIM_TYPEMOUSE) {
        // Raw mouse events are used for monitoring mouse from the background.
        // The events are to be sent to Pointer::Loggers
        RAWMOUSE& ev = raw_input->data.mouse;
        auto lock = window.Lock();
        auto& mouse = window.GetMouse();
        // LOG << "Mouse event: " << dump_struct(ev);
        using enum ui::PointerButton;
        using tuple = std::tuple<WORD, bool, PointerButton>;
        for (auto [flags, down, button] : {
                 tuple(RI_MOUSE_LEFT_BUTTON_DOWN, true, Left),
                 tuple(RI_MOUSE_LEFT_BUTTON_UP, false, Left),
                 tuple(RI_MOUSE_RIGHT_BUTTON_DOWN, true, Right),
                 tuple(RI_MOUSE_RIGHT_BUTTON_UP, false, Right),
                 tuple(RI_MOUSE_MIDDLE_BUTTON_DOWN, true, Middle),
                 tuple(RI_MOUSE_MIDDLE_BUTTON_UP, false, Middle),
                 tuple(RI_MOUSE_BUTTON_4_DOWN, true, Back),
                 tuple(RI_MOUSE_BUTTON_4_UP, false, Back),
                 tuple(RI_MOUSE_BUTTON_5_DOWN, true, Forward),
                 tuple(RI_MOUSE_BUTTON_5_UP, false, Forward),
             }) {
          if (ev.usButtonFlags & flags) {
            for (auto& logging : mouse.loggings) {
              if (down) {
                logging->logger.PointerLoggerButtonDown(*logging, button);
              } else {
                logging->logger.PointerLoggerButtonUp(*logging, button);
              }
            }
          }
        }
        if (ev.lLastX != 0 || ev.lLastY != 0) {
          for (auto& logging : mouse.loggings) {
            logging->logger.PointerLoggerMove(*logging, {(float)ev.lLastX, (float)ev.lLastY});
          }
        }
      } else {
        ERROR << "Received WM_INPUT event with type other than RIM_TYPEKEYBOARD & RIM_TYPEMOUSE";
        return DefWindowProc(hWnd, uMsg, wParam, lParam);
      }
      return DefWindowProc(hWnd, uMsg, wParam, lParam);
    }
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_CHAR:
      break;
    case WM_HOTKEY: {
      int id = wParam;  // discard the upper 32 bits
      // lParam carries the modifiers (first 16 bits) and then the keycode
      auto lock = window.Lock();
      automat::ui::OnHotKeyDown(id);
      break;
    }
    case WM_LBUTTONDOWN: {
      auto lock = window.Lock();
      window.GetMouse().ButtonDown(ui::PointerButton::Left);
      break;
    }
    case WM_LBUTTONUP: {
      auto lock = window.Lock();
      window.GetMouse().ButtonUp(ui::PointerButton::Left);
      break;
    }
    case WM_MBUTTONDOWN: {
      auto lock = window.Lock();
      window.GetMouse().ButtonDown(ui::PointerButton::Middle);
      break;
    }
    case WM_MBUTTONUP: {
      auto lock = window.Lock();
      window.GetMouse().ButtonUp(ui::PointerButton::Middle);
      break;
    }
    case WM_RBUTTONDOWN: {
      auto lock = window.Lock();
      window.GetMouse().ButtonDown(ui::PointerButton::Right);
      break;
    }
    case WM_RBUTTONUP: {
      auto lock = window.Lock();
      window.GetMouse().ButtonUp(ui::PointerButton::Right);
      break;
    }
    case WM_MOUSEMOVE: {
      auto lock = window.Lock();
      int16_t x = lParam & 0xFFFF;
      int16_t y = (lParam >> 16) & 0xFFFF;
      window.mouse_position.x = x + window.client_x;
      window.mouse_position.y = y + window.client_y;
      window.GetMouse().Move(window.ScreenToWindowPx(window.mouse_position));
      break;
    }
    case WM_MOUSELEAVE: {
      break;
    }
    case WM_MOUSEWHEEL: {
      int16_t x = lParam & 0xFFFF;
      int16_t y = (lParam >> 16) & 0xFFFF;
      int16_t delta = GET_WHEEL_DELTA_WPARAM(wParam);
      int16_t keys = GET_KEYSTATE_WPARAM(wParam);
      if (!touchpad::ShouldIgnoreScrollEvents()) {
        auto lock = window.Lock();
        window.GetMouse().Wheel(delta / 120.0);
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
      PostQuitMessage(0);
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

static WNDCLASSEX& GetWindowClass() {
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
                       .lpszClassName = kWindowName,
                       .hIconSm = LoadIcon(GetInstance(), (LPCTSTR)IDI_WINLOGO)};
    return wcex;
  }();
  return wcex;
}

unique_ptr<Window> Win32Window::Make(RootWidget& root, Status& status) {
  if (!RegisterClassEx(&GetWindowClass())) {
    AppendErrorMessage(status) += "Failed to register window class.";
    return nullptr;
  }
  // Save the window size and position - those values may be overwritten by WndProc when the
  // window is created.
  auto desired_size = root.size;
  Vec2 desired_pos = Vec2(root.output_device_x, root.output_device_y);
  bool maximized = root.maximized_horizontally || root.maximized_vertically;
  auto window = std::unique_ptr<Win32Window>(new Win32Window(root));
  window->hwnd =
      CreateWindowEx(WS_EX_OVERLAPPEDWINDOW, kWindowName, kWindowName, WS_OVERLAPPEDWINDOW, 0, 0,
                     CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr, GetInstance(), nullptr);
  if (!window->hwnd) {
    AppendErrorMessage(status) += "Failed to create main window.";
    return nullptr;
  }
  hwnd_to_window[window->hwnd] = window.get();
  root_widget->DisplayPixelDensity(win32::caps.px_per_meter);

  // Restore the position of the client area.
  if (!maximized && !isnan(desired_pos.x) && !isnan(desired_pos.y)) {
    int client_x, client_y;
    if (signbit(desired_pos.x)) {  // desired_pos.x is negative - distance from the right edge
      client_x = caps.screen_left_px + roundf(caps.screen_width_px +
                                              (desired_pos.x - desired_size.x) * caps.px_per_meter);
    } else {
      client_x = caps.screen_left_px + roundf(desired_pos.x * caps.px_per_meter);
    }
    if (signbit(desired_pos.y)) {  // desired_pos.y is negative - distance from the bottom edge
      client_y = caps.screen_top_px + roundf(caps.screen_height_px +
                                             (desired_pos.y - desired_size.y) * caps.px_per_meter);
    } else {
      client_y = caps.screen_top_px + roundf(desired_pos.y * caps.px_per_meter);
    }
    POINT zero = {0, 0};
    ClientToScreen(window->hwnd, &zero);
    int delta_x = client_x - zero.x;
    int delta_y = client_y - zero.y;
    SetWindowPos(window->hwnd, nullptr, delta_x, delta_y, 0, 0, SWP_NOZORDER | SWP_NOSIZE);
  }

  if (!maximized) {
    root.Resized(desired_size);
  }

  window->RegisterInput();
  return window;
}

void Win32Window::MainLoop() {
  ShowWindow(hwnd, (root.maximized_horizontally || root.maximized_vertically) ? SW_SHOWMAXIMIZED
                                                                              : SW_SHOW);
  UpdateWindow(hwnd);

  while (true) {
    MSG msg = {};
    switch (GetMessage(&msg, nullptr, 0, 0)) {
      case -1:  // error
        ERROR << "GetMessage failed: " << GetLastError();
      case 0:  // fallthrough to WM_QUIT
        // return (int)msg.wParam;
        return;
      default:
        // For some reason TranslateMessage generates CP-1250 characters instead
        // of UTF-8. TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
  }
}

void Win32Window::PostToMainLoop(function<void()> f) {
  if (this_thread::get_id() == automat::main_thread_id) {
    f();
    return;
  }
  PostMessage(hwnd, WM_USER, 0, (LPARAM) new function<void()>(std::move(f)));
}

ui::Pointer& Win32Window::GetMouse() {
  if (!mouse) {
    mouse = std::make_unique<Win32Pointer>(*root_widget, ScreenToWindowPx(mouse_position), *this);
    TRACKMOUSEEVENT track_mouse_event = {
        .cbSize = sizeof(TRACKMOUSEEVENT),
        .dwFlags = TME_LEAVE,
        .hwndTrack = hwnd,
    };
    TrackMouseEvent(&track_mouse_event);
  }
  return *mouse;
}

Vec2 Win32Window::ScreenToWindowPx(Vec2 screen) {
  Vec2 window = (screen - Vec2(client_x, client_y));
  return window;
}

Vec2 Win32Window::WindowPxToScreen(Vec2 window) { return window + Vec2(client_x, client_y); }

Optional<Vec2> Win32Window::MousePositionScreenPx() { return mouse_position; }

void Win32Window::OnRegisterInput(bool keylogging, bool pointerlogging) {
  vector<RAWINPUTDEVICE> rids;
  rids.emplace_back(touchpad::GetRAWINPUTDEVICE(hwnd));
  rids.emplace_back(RAWINPUTDEVICE{
      .usUsagePage = hid::UsagePage_GenericDesktop,
      .usUsage = hid::Usage_GenericDesktop_Keyboard,
      .dwFlags =
          (keylogging ? RIDEV_INPUTSINK
                      : (DWORD)0) |  // captures input even when the window is not in the foreground
          RIDEV_NOLEGACY,            // adds keyboard and also ignores legacy keyboard messages
      .hwndTarget = hwnd,
  });
  if (pointerlogging) {
    rids.emplace_back(RAWINPUTDEVICE{
        .usUsagePage = hid::UsagePage_GenericDesktop,
        .usUsage = hid::Usage_GenericDesktop_Mouse,
        .dwFlags = RIDEV_INPUTSINK,
        .hwndTarget = hwnd,
    });
  } else {
    rids.emplace_back(RAWINPUTDEVICE{
        .usUsagePage = hid::UsagePage_GenericDesktop,
        .usUsage = hid::Usage_GenericDesktop_Mouse,
        .dwFlags = RIDEV_REMOVE,
        .hwndTarget = nullptr,
    });
  }
  BOOL register_result = RegisterRawInputDevices(rids.data(), rids.size(), sizeof(RAWINPUTDEVICE));
  if (!register_result) {
    ERROR << "Failed to register raw input device: " << GetLastErrorStr();
  }
  keylogging_enabled = keylogging;
}

void Win32Window::RequestResize(Vec2 new_size) {
  int w = roundf(new_size.x * caps.px_per_meter);
  int h = roundf(new_size.y * caps.px_per_meter);

  // If the window is maximized and requested size is different, un-maximize it first.
  if (w == client_width && h == client_height) {
    return;
  }
  if (IsMaximized(hwnd)) {
    ShowWindow(hwnd, SW_RESTORE);
  }

  // Account for window border when calling SetWindowPos
  RECT client_rect, window_rect;
  GetClientRect(hwnd, &client_rect);
  GetWindowRect(hwnd, &window_rect);
  float vertical_frame_adjustment = (window_rect.bottom - window_rect.top) - client_rect.bottom;
  float horizontal_frame_adjustment = (window_rect.right - window_rect.left) - client_rect.right;
  h += vertical_frame_adjustment;
  w += horizontal_frame_adjustment;
  SetWindowPos(hwnd, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER);
  root.Resized(new_size);
}

void Win32Window::RequestMaximize(bool horiz, bool vert) {
  if (horiz || vert) {
    ShowWindow(hwnd, SW_MAXIMIZE);
  }
  root.Maximized(horiz, vert);
}
