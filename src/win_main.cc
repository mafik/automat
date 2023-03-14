#include "win_main.h"

#include "backtrace.h"
#include "log.h"
#include "vk.h"
#include "win.h"

#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/core/SkFont.h>
#include <include/core/SkGraphics.h>
#include <include/effects/SkRuntimeEffect.h>

#include <chrono>

using namespace automaton;

int window_width;
int window_height;

void OnResize(int w, int h) {
  window_width = w;
  window_height = h;
  if (auto err = vk::Resize(w, h); !err.empty()) {
    std::wstring werr(err.begin(), err.end());
    MessageBox(nullptr, werr.c_str(), L"ALERT", 0);
  }
}

using namespace std::chrono;

struct LoadAnimation {
  using T = time_point<system_clock>;
  T start = system_clock::now();
  T last = start;
  SkPaint paint;
  SkRect rect = SkRect::MakeXYWH(-10, -10, 20, 20);

  float base_scale_v = 1.0;
  float base_scale = 0;
  float base_rotation = 0;
  float ring_factor = 0;
  bool infinite = false;
  float alpha = 0;
  float alpha_v = 0;
  bool loaded = false;
  int loaded_stage = 0;
  bool done = false;

  LoadAnimation() {
    paint.setColor(SK_ColorBLACK);
    paint.setStroke(true);
    paint.setAntiAlias(true);
    paint.setStrokeWidth(1);
  }

  void OnPaint(SkCanvas &canvas) {
    if (done) {
      return;
    }
    T now = system_clock::now();
    double t = duration_cast<milliseconds>(now - start).count() / 1000.0;
    double dt = duration_cast<milliseconds>(now - last).count() / 1000.0;
    last = now;

    if (loaded) {
      loaded_stage += 1;
    }
    int rects_to_skip = loaded_stage / 7;

    //canvas.clear(SK_ColorWHITE);

    int cx = window_width / 2;
    int cy = window_height / 2;

    float rect_side = rect.width() - 2 * paint.getStrokeWidth() /
                                         2; // account for stroke width

    canvas.save();
    canvas.translate(cx, cy);

    base_rotation -= 0.2f;
    if (base_rotation < -360) {
      base_rotation += 360;
    }
    canvas.rotate(base_rotation);

    base_scale = 1 + cos(t) * 0.2;
    canvas.scale(base_scale, base_scale);

    if (--rects_to_skip < 0) {
      canvas.drawRect(rect, paint);
    }

    float ring_scale = 1.20f;
    float ring_rotation = 19;

    ring_factor += (1 - ring_factor) * 0.015;

    canvas.rotate(ring_rotation * ring_factor * alpha);
    canvas.scale(pow(ring_scale, ring_factor * alpha),
                 pow(ring_scale, ring_factor * alpha));
    if (--rects_to_skip < 0) {
      canvas.drawRect(rect, paint);
    }
    if (infinite) {
      alpha_v += (0.02 - alpha_v) * 0.01;
    }
    alpha += alpha_v;
    if (alpha < 0) {
      alpha += 1;
    }
    if (alpha > 1) {
      alpha -= 1;
    }

    int maxdim = std::max(window_width, window_height);
    float window_diag = sqrt(2) * maxdim;
    for (int i = 0; i < 25; ++i) {
      float s = pow(ring_scale, ring_factor);
      rect_side *= s;
      if (rect_side > window_diag) {
        infinite = true;
        break;
      }
      canvas.rotate(ring_rotation * ring_factor);
      canvas.scale(s, s);
      if (--rects_to_skip < 0) {
        canvas.drawRect(rect, paint);
      }
    }

    canvas.restore();
    if (rects_to_skip > 0) {
      done = true;
    }
  }
};

LoadAnimation anim;

void OnPaint(SkCanvas &canvas) {

  const char* sksl = R"(
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
        vec3 d = .5 - fragcoord.xy1 / iResolution.y;
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
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();

  SkRuntimeShaderBuilder builder(effect);
  builder.uniform("iResolution") = SkV3(window_width, window_height, 0);
  builder.uniform("iTime") = (float)(elapsed / 1000.0f);
  
  SkPaint p;
  p.setShader(builder.makeShader());
  canvas.drawPaint(p);

  anim.OnPaint(canvas);
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
    anim.loaded = true;
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
