#include "win_main.h"

#include "backtrace.h"
#include "log.h"
#include "vk.h"
#include "win.h"
#include "loading_animation.h"

#include <include/core/SkCanvas.h>
#include <include/core/SkGraphics.h>
#include <include/effects/SkRuntimeEffect.h>

#include <chrono>

using namespace automaton;

void OnResize(int w, int h) {
  window_width = w;
  window_height = h;
  if (auto err = vk::Resize(w, h); !err.empty()) {
    std::wstring werr(err.begin(), err.end());
    MessageBox(nullptr, werr.c_str(), L"ALERT", 0);
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

void OnPaint(SkCanvas &canvas) {
  if (anim) {
    anim.OnPaint(canvas, Dream);
  } else {
    Dream(canvas);
  }
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
    OnPaint(*canvas);
    vk::Present();
    EndPaint(hWnd, &ps);
    break;
  }
  case WM_MBUTTONDOWN:
    break;
  case WM_DESTROY:
    PostQuitMessage(0);
    break;
  default:
    return DefWindowProc(hWnd, uMsg, wParam, lParam);
    break;
  }
  return 0;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                    PWSTR pCmdLine, int nCmdShow) {
  EnableBacktraceOnSIGSEGV();
  SetConsoleOutputCP(CP_UTF8); // utf-8
  SkGraphics::Init();

  if (!RegisterClassEx(&GetWindowClass())) {
    FATAL() << "Failed to register window class.";
  }

  main_window = CreateAutomatonWindow();
  if (!main_window) {
    FATAL() << "Failed to create main window.";
  }

  if (auto err = vk::Init(); !err.empty()) {
    FATAL() << "Failed to initialize Vulkan: " << err;
  }

  ShowWindow(main_window, nCmdShow);
  UpdateWindow(main_window);

  RECT rect;
  GetClientRect(main_window, &rect);
  OnResize(rect.right - rect.left, rect.bottom - rect.top);

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
