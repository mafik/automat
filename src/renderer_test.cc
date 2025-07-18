// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include "renderer.hh"

#include <include/core/SkGraphics.h>
#include <include/core/SkMatrix.h>

#include <thread>

#include "animation.hh"
#include "drawable_rtti.hh"
#include "global_resources.hh"
#include "gtest.hh"
#include "root_widget.hh"
#include "textures.hh"
#include "thread_name.hh"
#include "time.hh"
#include "vk.hh"
#include "xcb_window.hh"

using namespace automat;
using namespace automat::gui;
using namespace std;

constexpr bool kPowersave = true;
time::SystemPoint next_frame;

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
      double frame_count =
          ceil(time::ToSeconds(now - next_frame) * root_widget->window->screen_refresh_rate);
      next_frame += time::FromSeconds(frame_count / root_widget->window->screen_refresh_rate);
      constexpr bool kLogSkippedFrames = false;
      if (kLogSkippedFrames && frame_count > 1) {
        LOG << "Skipped " << (uint64_t)(frame_count - 1) << " frames";
      }
    } else {
      // This normally sleeps until T + ~10ms.
      // With timeBeginPeriod(1) it's T + ~1ms.
      // TODO: try condition_variable instead
      std::this_thread::sleep_until(next_frame);
      next_frame += time::FromSeconds(1.0 / root_widget->window->screen_refresh_rate);
    }
  }

  {
    auto lock = root_widget->window->Lock();
    Vec2 size_px = Vec2(root_widget->window->client_width, root_widget->window->client_height);
    if (root_widget->window->vk_size != size_px) {
      LOG << "Resizing window to " << size_px;
      if (auto err =
              vk::Resize(root_widget->window->client_width, root_widget->window->client_height);
          !err.empty()) {
        FATAL << "Couldn't set window size to " << root_widget->window->client_width << "x"
              << root_widget->window->client_height << ": " << err;
      }
      root_widget->window->vk_size = size_px;
    }
  }

  SkCanvas* canvas = vk::GetBackbufferCanvas();
  if (canvas == nullptr) {
    return;
  }
  RenderFrame(*canvas);
}

void RenderThread(std::stop_token stop_token) {
  SetThreadName("Render Thread");
  while (!stop_token.stop_requested()) {
    VulkanPaint();
    if (auto automat_image_provider = static_cast<AutomatImageProvider*>(image_provider.get())) {
      automat_image_provider->TickCache();
    }
  }
}

std::jthread render_thread;

time::SteadyPoint test_start;

std::string FormatTime(double d) { return f("{:.3f}s", d); }

std::string FormatTime(time::Duration d) { return FormatTime(time::ToSeconds(d)); }

std::string FormatTime(time::Timer& timer) { return FormatTime(timer.now - test_start); }

struct SlowDrawable : SkDrawableRTTI {
  SkRect onGetBounds() override { return SkRect::MakeWH(100, 100); }
  void onDraw(SkCanvas* canvas) override {
    LOG << "SlowDrawable::onDraw";
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  }
  const char* getTypeName() const override { return "SlowDrawable"; }
  void flatten(SkWriteBuffer&) const override {}
};

struct SlowWidget : Widget {
  sk_sp<SkDrawable> drawable;
  sk_sp<SkRuntimeEffect> runtime_effect;
  SkPaint paint;
  SlowWidget() : drawable(SkDrawableRTTI::Make<SlowDrawable>()) {
    Status status;
    runtime_effect = resources::CompileShader(
        fs::VFile{
            .path = "slow_shader.sksl",
            .content = R"(// kind=shader
uniform float iTime;


float hash(float n) {
    return fract(sin(n + iTime) * 43758.5453);
}

vec4 main( float2 fragCoord ) {

    float3 col = float3(hash(fragCoord.x), hash(fragCoord.y), hash(iTime));

    for (int i = 0; i < 1000; ++i) {
      for (int j = 0; j < 2000; ++j) {
        col = float3(hash(col.y), hash(col.z), hash(col.x));
      }
    }

    return vec4(col, 1.0);
}
)",
        },
        status);
    if (!OK(status)) {
      FATAL << "Failed to compile shader: " << status;
    }
  }
  animation::Phase Tick(time::Timer& timer) override {
    LOG << FormatTime(timer.d) << " SlowWidget::Tick";
    local_to_parent =
        SkM44(SkMatrix::RotateDeg(fmod(timer.NowSeconds() * 360 / 5, 360), root_widget->size / 2));

    float time = time::ToSeconds(timer.now - test_start);
    auto uniforms = SkData::MakeWithCopy(&time, sizeof(time));
    paint.setShader(runtime_effect->makeShader(uniforms, nullptr, 0));
    return animation::Animating;
  }
  void Draw(SkCanvas& canvas) const override {
    auto shape = Shape();
    canvas.drawPath(shape, paint);
    // canvas.drawDrawable(drawable.get());
  }
  SkPath Shape() const override {
    auto r =
        Rect::MakeAtZero(1_cm, 2_cm).MoveBy(root_widget->TextureBounds()->Center() + Vec2(0, 3_cm));
    return SkPath::Oval(r);
  };
};

// Test case flow:
// 1. Root widget => Super slow widget (slow to render)
// 2. Render the initial frame (expect it to take a long time)
// 3. Animate the super slow widget's position & scale
// 4. All of the subsequent frames should be fast
TEST(Renderer, Construction) {
  SkGraphics::Init();
  root_widget = MakePtr<RootWidget>();
  Status status;
  root_widget->window = xcb::XCBWindow::Make(*root_widget, status);
  if (auto err = vk::Init(); !err.empty()) {
    FATAL << "Failed to initialize Vulkan: " << err;
  }
  RendererInit();
  std::stop_source stop_source;
  root_widget->children.push_back(MakePtr<SlowWidget>());
  test_start = time::SteadyNow();
  render_thread = std::jthread(RenderThread, stop_source.get_token());
  LOG << "Render thread started";
  root_widget->window->MainLoop();
  // std::this_thread::sleep_for(std::chrono::seconds(10));
  stop_source.request_stop();
  render_thread.join();
  LOG << "Render thread stopped";
  root_widget->ForgetParents();
  root_widget.reset();
  resources::Release();
  image_provider.reset();
  RendererShutdown();
  Widget::CheckAllWidgetsReleased();

  vk::Destroy();
  LOG << "Exiting.";
}
