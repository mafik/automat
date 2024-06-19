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

#include "backtrace.hh"
#include "hid.hh"
#include "library.hh"  // IWYU pragma: export
#include "loading_animation.hh"
#include "path.hh"
#include "root.hh"
#include "thread_name.hh"
#include "touchpad.hh"
#include "vk.hh"
#include "win.hh"
#include "win_key.hh"
#include "win_main.hh"
#include "window.hh"

using namespace automat;

HWND main_window;

// This is based on the scaling configured by the user in Windows settings.
// It's not used anywhere by Automat.
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
float screen_width_m = (float)screen_width_px / USER_DEFAULT_SCREEN_DPI * kMetersPerInch;
float screen_height_m = (float)screen_height_px / USER_DEFAULT_SCREEN_DPI * kMetersPerInch;

Vec2 mouse_position;

float DisplayPxPerMeter() { return screen_width_px / screen_width_m; }
Vec2 WindowSize() { return Vec2(window_width, window_height) / DisplayPxPerMeter(); }

// Coordinate spaces:
// - Screen (used by Windows & win_main.cc, origin in the top left corner of the
// screen, Y axis goes down)
// - Window (used for win_main.cc <=> gui.cc communication, origin in the bottom
// left corner of the window, Y axis goes up)
// - Canvas (used for gui.cc <=> Automat communication, origin in the center
// of the window, Y axis goes up)

namespace automat::gui {

Vec2 ScreenToWindow(Vec2 screen) {
  Vec2 window = (screen - Vec2(client_x, client_y + window_height)) / DisplayPxPerMeter();
  window.y = -window.y;
  return window;
}

Vec2 WindowToScreen(Vec2 window) {
  window.y = -window.y;
  return window * DisplayPxPerMeter() + Vec2(client_x, window_height + client_y);
}

Vec2 GetMainPointerScreenPos() { return mouse_position; }

}  // namespace automat::gui

std::unique_ptr<gui::Window> window;
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
  if (auto err = vk::Resize(window_width, window_height); !err.empty()) {
    MessageBox(nullptr, err.c_str(), "ALERT", 0);
  }
}

void Paint(SkCanvas& canvas) {
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
      float diag =
          sqrt(screen_height_m * screen_height_m + screen_width_m * screen_width_m) / 0.0254f;
      LOG << "Display: " << f("%.1f", diag) << "″ " << int(screen_width_m * 1000) << "x"
          << int(screen_height_m * 1000) << "mm (" << screen_width_px << "x" << screen_height_px
          << "px) " << screen_refresh_rate << "Hz";
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

std::string StatePath() { return Path::ExecutablePath().Parent() / "automat_state.json"; }

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
      window_width = LOWORD(lParam);
      window_height = HIWORD(lParam);
      ResizeVulkan();
      if (window) {
        window->Resize(WindowSize());
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
        key_state[virtual_key] = 0x80;
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
          LOG << "Sending key down event " << window_active;
          if (window_active) {
            gui::keyboard->KeyDown(key);
          }
        }
      } else if (ev.Message == WM_KEYUP) {
        key_state[virtual_key] = 0;
        // Right Alt sends WM_KEYDOWN for Control & Alt, but WM_KEYUP for Alt only.
        // This is a workaround for that.
        if (virtual_key == 0x12) {
          key_state[0x11] = 0;
        }
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
      // Write window_state to a temp file
      auto state_path = StatePath();
      rapidjson::StringBuffer sb;
      rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(sb);
      writer.SetMaxDecimalPlaces(6);
      writer.StartObject();
      writer.Key("version");
      writer.Uint(1);
      writer.Key("maximized");
      writer.Bool(IsMaximized(main_window));
      writer.Key("window");
      window->SerializeState(writer);
      root_machine->SerializeState(writer, "root");

      writer.EndObject();
      writer.Flush();
      std::string window_state = sb.GetString();
      HANDLE file =
          CreateFile(state_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
      if (file != INVALID_HANDLE_VALUE) {
        DWORD bytes_written;
        WriteFile(file, window_state.c_str(), window_state.size(), &bytes_written, nullptr);
        CloseHandle(file);
        LOG << "Saved window state to " << state_path;
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

static void DeserializeState(Deserializer& d, Status& status) {
  for (auto& key : ObjectView(d, status)) {
    if (key == "version") {
      int version = d.GetInt(status);
      if (!OK(status)) {
        AppendErrorMessage(status) += "Failed to deserialize version";
        return;
      } else if (version != 1) {
        AppendErrorMessage(status) += "Unsupported version: " + std::to_string(version);
        return;
      }
    } else if (key == "window") {
      window->DeserializeState(d, status);
      if (!OK(status)) {
        AppendErrorMessage(status) += "Failed to deserialize window state";
        return;
      }
    } else if (key == "maximized") {
      bool maximized = d.GetBool(status);
      if (!OK(status)) {
        AppendErrorMessage(status) += "Failed to deserialize maximized state";
        return;
      } else {
        if (maximized) {
          ShowWindow(main_window, SW_MAXIMIZE);
        }
      }
    } else if (key == "root") {
      root_machine->DeserializeState(root_location, d);
    } else {
      AppendErrorMessage(status) += "Unexpected key: " + key;
      return;
    }
  }
  bool fully_decoded = d.reader.IterativeParseComplete();
  if (!fully_decoded) {
    AppendErrorMessage(status) += "Extra data at the end of the JSON string, " + d.ErrorContext();
    return;
  }
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

  InitRoot();
  anim.LoadingCompleted();

  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  SkGraphics::Init();

  if (!RegisterClassEx(&GetWindowClass())) {
    FATAL << "Failed to register window class.";
  }

  main_window = CreateAutomatWindow();
  if (!main_window) {
    FATAL << "Failed to create main window.";
  }

  RegisterRawInput();

  QueryDisplayCaps();

  if (auto err = vk::Init(); !err.empty()) {
    FATAL << "Failed to initialize Vulkan: " << err;
  }

  next_frame = time::SystemNow();
  ShowWindow(main_window, nCmdShow);
  UpdateWindow(main_window);
  RECT rect;
  GetClientRect(main_window, &rect);
  window_width = rect.right - rect.left;
  window_height = rect.bottom - rect.top;
  window.reset(new gui::Window(WindowSize(), DisplayPxPerMeter()));
  window->RequestResize = [&](Vec2 new_size) {
    int w = roundf(new_size.x * DisplayPxPerMeter());
    int h = roundf(new_size.y * DisplayPxPerMeter());

    // If the window is maximized and requested size is different, un-maximize it first.
    if (w == window_width && h == window_height) {
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

  {
    auto state_path = StatePath();
    HANDLE file =
        CreateFile(state_path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
      DWORD file_size = GetFileSize(file, nullptr);
      auto json = std::string(file_size, '\0');
      DWORD bytes_read;
      ReadFile(file, json.data(), file_size, &bytes_read, nullptr);
      CloseHandle(file);

      rapidjson::StringStream stream(json.c_str());
      Deserializer deserializer(stream);
      Status status;
      DeserializeState(deserializer, status);
      if (!OK(status)) {
        ERROR << "Failed to deserialize saved state: " << status;
      }
    }
  }
  gui::keyboard = std::make_unique<gui::Keyboard>(*window);
  ResizeVulkan();
  RenderingStart();

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
