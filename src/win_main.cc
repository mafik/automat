#undef NOGDI
#include <windows.h>
#undef ERROR
#define NOGDI

#include "win_main.h"

#include "backtrace.h"
#include "base.h"
#include "loading_animation.h"
#include "log.h"
#include "vk.h"
#include "win.h"

#include <atomic>
#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/core/SkGraphics.h>
#include <include/effects/SkRuntimeEffect.h>

#include <chrono>
#include <windef.h>
#include <winuser.h>

namespace automaton {

HWND main_window;
int main_window_dpi = USER_DEFAULT_SCREEN_DPI;
int window_width;
int window_height;

vec2 camera_position;
constexpr float m_per_inch = 0.0254f;

// Placeholder values for screen size. They should be updated when window is
// resized.
int screen_width_px = 1920;
int screen_height_px = 1080;
float screen_width_m =
    (float)screen_width_px / USER_DEFAULT_SCREEN_DPI * m_per_inch;
float screen_height_m =
    (float)screen_height_px / USER_DEFAULT_SCREEN_DPI * m_per_inch;
float zoom = 1.0;

float PxPerM() {
  return screen_width_px / screen_width_m * zoom;
}

float TrueDPI() {
  return PxPerM() * m_per_inch;
}

SkRect GetCameraRect() {
  return SkRect::MakeXYWH(camera_position.X - window_width / PxPerM() / 2,
                          camera_position.Y - window_height / PxPerM() / 2,
                          window_width / PxPerM(), window_height / PxPerM());
}

SkPaint &GetBackgroundPaint() {
  static SkRuntimeShaderBuilder builder = []() {
    const char *sksl = R"(
      uniform float px_per_m;

      float4 bg = float4(0.05, 0.05, 0.00, 1);
      float4 fg = float4(0.0, 0.32, 0.8, 1);

      float grid(vec2 coord_m, float dots_per_m, float r) {
        vec2 grid_coord = fract(coord_m * dots_per_m + 0.5) - 0.5;
        return smoothstep(r, r - 1/px_per_m, length(grid_coord) / dots_per_m);
      }

      half4 main(vec2 fragcoord) {
        float dm_grid = grid(fragcoord, 10, 3/px_per_m);
        float cm_grid = grid(fragcoord, 100, 3/px_per_m) * 0.5;
        float mm_grid = grid(fragcoord, 1000, 2/px_per_m) * 0.25;
        float d = max(max(mm_grid, cm_grid), dm_grid);
        return mix(bg, fg, d);
      }
    )";

    auto [effect, err] = SkRuntimeEffect::MakeForShader(SkString(sksl));
    if (!err.isEmpty()) {
      FATAL() << err.c_str();
    }
    SkRuntimeShaderBuilder builder(effect);
    return builder;
  }();
  static SkPaint paint;
  builder.uniform("px_per_m") = PxPerM();
  paint.setShader(builder.makeShader());
  return paint;
}

SkColor background_color = SkColorSetRGB(0x0f, 0x0f, 0x0e);
SkColor tick_color = SkColorSetRGB(0x00, 0x53, 0xce);

constexpr uint8_t kScanCodeW = 0x11;
constexpr uint8_t kScanCodeA = 0x1e;
constexpr uint8_t kScanCodeS = 0x1f;
constexpr uint8_t kScanCodeD = 0x20;

struct Timer {
  using T = double;
  using duration = std::chrono::duration<T>;
  using clock = std::chrono::system_clock;
  using time_point = std::chrono::time_point<clock, duration>;
  time_point now = clock::now();
  time_point last = now;
  T d = 0; // delta from last frame
  void Tick() {
    now = clock::now();
    d = (now - last).count();
    last = now;
  }
};

struct Win32Client : Object {
  static const Win32Client proto;
  channel canvas_in;
  channel canvas_out;
  bool pressed_scancodes[256] = {};
  Timer t;

  string_view Name() const override { return "Win32Client"; }
  std::unique_ptr<Object> Clone() const override {
    return std::make_unique<Win32Client>();
  }
  void Run(Location &here) override {
    SkCanvas *canvas = (SkCanvas *)canvas_in.recv();
    t.Tick();

    if (pressed_scancodes[kScanCodeW]) {
      camera_position.Y += 0.1 * t.d;
    }
    if (pressed_scancodes[kScanCodeS]) {
      camera_position.Y -= 0.1 * t.d;
    }
    if (pressed_scancodes[kScanCodeA]) {
      camera_position.X -= 0.1 * t.d;
    }
    if (pressed_scancodes[kScanCodeD]) {
      camera_position.X += 0.1 * t.d;
    }

    canvas->save();
    canvas->translate(window_width / 2., window_height / 2.);
    canvas->scale(PxPerM(), -PxPerM());
    canvas->translate(-camera_position.X, -camera_position.Y);

    canvas->drawPaint(GetBackgroundPaint());

    canvas->restore();

    canvas_out.send_force(canvas);
  }
};
const Win32Client Win32Client::proto;

Location root_location = Location(nullptr);
Machine &root_machine = *root_location.Create<Machine>();
Location &win32_client_location = root_machine.Create<Win32Client>();
Win32Client &win32_client = *win32_client_location.ThisAs<Win32Client>();
bool automaton_running = false;

} // namespace automaton

using namespace automaton;

void OnResize(int w, int h) {
  window_width = w;
  window_height = h;
  if (auto err = vk::Resize(w, h); !err.empty()) {
    MessageBox(nullptr, err.c_str(), "ALERT", 0);
  }
}

void Checkerboard(SkCanvas &canvas) {
  SkISize size = canvas.getBaseLayerSize();
  SkPaint paint;
  paint.setColor(SK_ColorLTGRAY);
  SkRect rect;
  for (int x = 0; x < size.fWidth; x += 20) {
    for (int y = (x / 20 % 2) * 20; y < size.fHeight; y += 40) {
      rect.setXYWH(x, y, 20, 20);
      canvas.drawRect(rect, paint);
    }
  }
}

void Dream(SkCanvas &canvas) {
  const char *sksl = R"(
    uniform float3 iResolution;      // Viewport resolution (pixels)
    uniform float  iTime;            // Shader playback time (s)

    // Source: @notargs https://twitter.com/notargs/status/1250468645030858753
    float f(vec3 p) {
        p.z -= iTime * 10.;
        float a = p.z * .1;
        p.xy *= mat2(cos(a), sin(a), -sin(a), cos(a));
        return .1 - length(cos(p.xy) + sin(p.yz));
    }

    half4 main(vec2 fragcoord) { 
        vec3 d = (0.5 * iResolution.xyy - fragcoord.xy1) / iResolution.y;
        vec3 p=vec3(0);
        for (int i = 0; i < 32; i++) {
          p += f(p) * d;
        }
        return ((sin(p) + vec3(2, 5, 12)) / length(p)).xyz1;
    }
  )";

  auto [effect, err] = SkRuntimeEffect::MakeForShader(SkString(sksl));
  if (!err.isEmpty()) {
    LOG() << err.c_str();
  }

  static auto start = std::chrono::system_clock::now();
  auto now = std::chrono::system_clock::now();
  auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - start)
          .count();

  SkRuntimeShaderBuilder builder(effect);
  builder.uniform("iResolution") = SkV3(window_width, window_height, 0);
  builder.uniform("iTime") = (float)(elapsed / 1000.0f);

  SkPaint p;
  p.setShader(builder.makeShader());
  canvas.drawPaint(p);
}

void PaintAutomaton(SkCanvas &canvas) {
  win32_client.canvas_in.send_force(&canvas);
  events.send(std::make_unique<RunTask>(&win32_client_location));
  // We're waiting for the canvas to be sent back - it means that drawing has
  // completed. We can drop the received canvas because it's the same as the one
  // we sent.
  win32_client.canvas_out.recv();
}

void PaintLoadingAnimation(SkCanvas &canvas) {
  auto Paint = automaton_running ? PaintAutomaton : Dream;
  if (anim) {
    anim.OnPaint(canvas, Paint);
  } else {
    Paint(canvas);
  }
}

void RunOnAutomatonThread(std::function<void()> f) {
  events.send(std::make_unique<FunctionTask>(&win32_client_location,
                                             [f](Location &l) { f(); }));
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  switch (uMsg) {
  case WM_SIZE:
    OnResize(LOWORD(lParam), HIWORD(lParam));
    break;
  case WM_PAINT: {
    PAINTSTRUCT ps;
    BeginPaint(hWnd, &ps);
    SkCanvas *canvas = vk::GetBackbufferCanvas();
    PaintLoadingAnimation(*canvas);
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
  case WM_MBUTTONDOWN:
    break;
  case WM_KEYDOWN: {
    uint8_t key = (uint8_t)wParam;             // layout-dependent key code
    uint8_t scan_code = (lParam >> 16) & 0xFF; // identifies the physical key
    RunOnAutomatonThread(
        [scan_code]() { win32_client.pressed_scancodes[scan_code] = true; });
    break;
  }
  case WM_KEYUP: {
    uint8_t key = (uint8_t)wParam;
    uint8_t scan_code = (lParam >> 16) & 0xFF;
    RunOnAutomatonThread(
        [scan_code]() { win32_client.pressed_scancodes[scan_code] = false; });
    break;
  }
  case WM_CHAR: {
    uint8_t utf8_char = (uint8_t)wParam;
    uint8_t scan_code = (lParam >> 16) & 0xFF;
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
  OnResize(rect.right - rect.left, rect.bottom - rect.top);

  std::thread(RunThread).detach();
  automaton_running = true;

  anim.LoadingCompleted();
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
  return (int)msg.wParam;
}
