#pragma once

#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/effects/SkRuntimeEffect.h>

#include <chrono>
#include <functional>

namespace automaton {

struct LoadingAnimation {
  using duration = std::chrono::duration<double>;
  using time_point =
      std::chrono::time_point<std::chrono::system_clock, duration>;
  time_point start = std::chrono::system_clock::now();
  time_point now = start;
  time_point last = start;
  duration t = duration(0);
  duration dt = duration(0);

  enum State { kPreLoading, kLoading, kPostLoading, kDone } state = kPreLoading;

  void LoadingCompleted() { state = kPostLoading; }

  operator bool() { return state != kDone; }

  void OnPaint(SkCanvas &canvas, std::function<void(SkCanvas &)> paint);

  virtual void PrePaint(SkCanvas &canvas) {
    canvas.saveLayer(nullptr, nullptr);
  }

  virtual void PostPaint(SkCanvas &canvas) { canvas.restore(); }
};

SkColor SkColorFromLittleEndian(uint32_t little_endian_rgb);

struct HypnoRect : public LoadingAnimation {
  SkPaint paint;
  SkRect rect = SkRect::MakeXYWH(-10, -10, 20, 20);

  float unfold = 0;
  float first_twist = 0;
  float first_twist_v = 0;

  float base_twist = 0;
  float base_twist_v = 0;

  const float kScalePerTwist = 1.20f;
  const float kDegreesPerTwist = 19;

  const char *sksl = R"(
    uniform float2 resolution;
    uniform float4 top_color;
    uniform float4 bottom_color;
    half4 main(vec2 fragcoord) {
      float2 uv = sk_FragCoord.xy / resolution;
      return mix(bottom_color, top_color, 1 - uv.y);
    }
  )";
  std::unique_ptr<SkRuntimeShaderBuilder> shader_builder;

  SkColor kTopColor = SkColorFromLittleEndian(0x16389d);
  SkColor kBottomColor = SkColorFromLittleEndian(0x142261);
  SkColor kBackgroundColor = SkColorFromLittleEndian(0x111616);

  HypnoRect();

  float Twist(SkCanvas &canvas, float factor);

  void PrePaint(SkCanvas &canvas) override;

  void PostPaint(SkCanvas &canvas) override;
};

extern HypnoRect anim;

} // namespace automaton