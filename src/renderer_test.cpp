// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "renderer.hpp"

#include <include/core/SkGraphics.h>
#include <include/core/SkMatrix.h>

#include <thread>

#include "animation.hpp"
#include "automat.hpp"
#include "drawable_rtti.hpp"
#include "global_resources.hpp"
#include "gtest.hpp"
#include "prototypes.hpp"
#include "root_widget.hpp"
#include "textures.hpp"
#include "time.hpp"
#include "vk.hpp"

using namespace automat;
using namespace automat::ui;
using namespace std;

time::SteadyPoint test_start;

std::string FormatTime(double d) { return f("{:.3f}s", d); }

std::string FormatTime(time::Duration d) { return FormatTime(time::ToSeconds(d)); }

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
  SlowWidget(Widget* parent) : Widget(parent), drawable(SkDrawableRTTI::Make<SlowDrawable>()) {
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
  Tock Tick(time::Timer& timer) override {
    LOG << FormatTime(timer.d) << " SlowWidget::Tick";
    local_to_parent =
        SkM44(SkMatrix::RotateDeg(fmod(timer.NowSeconds() * 360 / 5, 360), root_widget->size / 2));

    float time = time::ToSeconds(timer.now - test_start);
    auto uniforms = SkData::MakeWithCopy(&time, sizeof(time));
    paint.setShader(runtime_effect->makeShader(uniforms, nullptr, 0));
    return Tock::Drawing;
  }
  void Draw(SkCanvas& canvas) const override {
    auto shape = Shape();
    canvas.drawPath(shape, paint);
    // canvas.drawDrawable(drawable.get());
  }
  SkPath Shape() const override {
    auto r =
        Rect::MakeAtZero(1_cm, 2_cm).MoveBy(root_widget->DrawBounds()->Center() + Vec2(0, 3_cm));
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
  prototypes.emplace();
  root_widget = make_unique<RootWidget>();
  new SlowWidget(root_widget.get());  // parents itself into root_widget's layers
  test_start = time::SteadyNow();
  root_widget->Init();  // starts the render thread, so SlowWidget must already exist
  root_widget->window->MainLoop(automat::stop_source.get_token());
  automat::stop_source.request_stop();
  root_widget.reset();
  prototypes.reset();
  resources::Release();
  image_provider.reset();
  PersistentImage::ReleaseAll();
  RendererShutdown();
  Widget::CheckAllWidgetsReleased();

  vk::Destroy();
  LOG << "Exiting.";
}
