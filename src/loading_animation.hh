// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/effects/SkRuntimeEffect.h>

#include "animation.hh"
#include "color.hh"
#include "time.hh"
#include "units.hh"

namespace automat {

struct LoadingAnimation {
  time::SteadyPoint start = time::SteadyNow();

  // Each loading animation can be in a number of states.
  enum State {
    kPreLoading,   // Initial sequence, happens only once when animation starts up
    kLoading,      // Continuous loading animation
    kPostLoading,  // Loading has finished and the animation is disappearing
    kDone          // Animation has completely disappeared
  } state = kPreLoading;

  void LoadingCompleted() { state = kPostLoading; }

  operator bool() { return state != kDone; }

  virtual animation::Phase Tick(time::Timer& timer) {
    return state == kDone ? animation::Finished : animation::Animating;
  }
  virtual void PreDraw(SkCanvas& canvas) { canvas.saveLayer(nullptr, nullptr); }
  virtual void PostDraw(SkCanvas& canvas) { canvas.restore(); }

  struct DrawGuard {
    SkCanvas& canvas;
    LoadingAnimation& anim;
    DrawGuard(SkCanvas& canvas, LoadingAnimation& anim) : canvas(canvas), anim(anim) {
      anim.PreDraw(canvas);
    }
    ~DrawGuard() { anim.PostDraw(canvas); }
  };

  DrawGuard WrapDrawing(SkCanvas& canvas) { return DrawGuard(canvas, *this); }
};

struct HypnoRect : public LoadingAnimation {
  SkPaint paint;
  Rect rect = Rect::MakeAtZero(1_cm, 1_cm);

  float unfold = 0;
  float first_twist = 0;
  float first_twist_v = 0;

  float base_twist = 0;
  float base_twist_v = 0;

  float base_scale = 1;
  time::T t = 0;
  int client_width = 100;   // px
  int client_height = 100;  // px
  float client_diag = 144;  // px

  constexpr static float kScalePerTwist = 1.20f;
  constexpr static float kDegreesPerTwist = 19;

  // SkColor kTopColor = "#16389d"_color;
  // SkColor kBottomColor = "#142261"_color;
  SkColor top_color = "#ff389d"_color;
  SkColor bottom_color = "#14ff10"_color;
  SkColor background_color = "#111616"_color;

  HypnoRect();

  animation::Phase Tick(time::Timer&) override;
  void PreDraw(SkCanvas&) override;
  void PostDraw(SkCanvas&) override;
};

extern HypnoRect anim;

}  // namespace automat