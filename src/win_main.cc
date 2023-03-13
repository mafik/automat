#include "win_main.h"

#include "backtrace.h"
#include "log.h"
#include "vk.h"
#include "win.h"

#include <include/core/SkCanvas.h>
#include <include/core/SkFont.h>
#include <include/core/SkGraphics.h>
#include <include/effects/SkGradientShader.h>

using namespace automaton;

void OnResize(int w, int h) {
  if (auto err = vk::Resize(w, h); !err.empty()) {
    std::wstring werr(err.begin(), err.end());
    MessageBox(nullptr, werr.c_str(), L"ALERT", 0);
  }
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  switch (uMsg) {
  case WM_SIZE:
    OnResize(LOWORD(lParam), HIWORD(lParam));
    break;
  case WM_PAINT: {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hWnd, &ps);

    SkCanvas* canvas = vk::GetBackbufferCanvas();

    // Clear background
    canvas->clear(SK_ColorWHITE);

    SkPaint paint;
    paint.setColor(SK_ColorRED);

    // Draw a rectangle with red paint
    SkRect rect = SkRect::MakeXYWH(10, 10, 128, 128);
    canvas->drawRect(rect, paint);

    // Set up a linear gradient and draw a circle
    {
      SkPoint linearPoints[] = {{0, 0}, {300, 300}};
      SkColor linearColors[] = {SK_ColorGREEN, SK_ColorBLACK};
      paint.setShader(SkGradientShader::MakeLinear(
          linearPoints, linearColors, nullptr, 2, SkTileMode::kMirror));
      paint.setAntiAlias(true);

      canvas->drawCircle(200, 200, 64, paint);

      // Detach shader
      paint.setShader(nullptr);
    }

    // Draw a message with a nice black paint
    SkFont font;
    font.setSubpixel(true);
    font.setSize(20);
    paint.setColor(SK_ColorBLACK);

    canvas->save();
    static const char message[] = "Hello World ";

    // Translate and rotate
    canvas->translate(300, 300);
    static double rotation_angle = 0;
    rotation_angle += 0.2f;
    if (rotation_angle > 360) {
      rotation_angle -= 360;
    }
    canvas->rotate(rotation_angle);

    // Draw the text
    canvas->drawSimpleText(message, strlen(message), SkTextEncoding::kUTF8, 0,
                           0, font, paint);

    canvas->restore();

    vk::Present();


    EndPaint(hWnd, &ps);
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
  while (GetMessage(&msg, nullptr, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }

  vk::Destroy();
  return (int)msg.wParam;
}
