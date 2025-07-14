// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#include "library_mouse_move.hh"

#include <include/core/SkBlendMode.h>

#include <ranges>

#if defined(__linux__)
#include <xcb/xproto.h>
#include <xcb/xtest.h>
#endif

#include "concurrentqueue.hh"
#include "embedded.hh"
#include "global_resources.hh"
#include "library_mouse.hh"
#include "math.hh"
#include "textures.hh"
#include "widget.hh"

#if defined(__linux__)
#include "xcb.hh"
#endif

using namespace automat::gui;

namespace automat::library {

string_view MouseMove::Name() const { return "Mouse Move"; }

Ptr<Object> MouseMove::Clone() const { return MakePtr<MouseMove>(); }

PersistentImage dpad_image = PersistentImage::MakeFromAsset(
    embedded::assets_mouse_dpad_webp, PersistentImage::MakeArgs{.scale = mouse::kTextureScale});

// A turtle with a pixelated cursor on its back
struct MouseMoveWidget : Object::FallbackWidget {
  moodycamel::ConcurrentQueue<Vec2> trail;
  moodycamel::ProducerToken trail_token;
  std::deque<Vec2> trail_draw;

  MouseMoveWidget(WeakPtr<MouseMove>&& weak_mouse_move) : trail(64, 1, 0), trail_token(trail) {
    object = std::move(weak_mouse_move);
  }
  SkPath Shape() const override {
    Rect bounds = *TextureBounds();
    float width = bounds.Width();
    SkRRect rrect;
    SkVector radii[4] = {
        {width / 2, width / 2},
        {width / 2, width / 2},
        {width / 3, width / 3},
        {width / 3, width / 3},
    };
    rrect.setRectRadii(bounds.sk, radii);
    return SkPath::RRect(rrect);
  }
  animation::Phase Tick(time::Timer& timer) override {
    auto phase = animation::Finished;
    size_t dequeued;
    do {
      Vec2 buf[32];
      dequeued = trail.try_dequeue_bulk_from_producer(trail_token, buf, std::size(buf));
      for (size_t i = 0; i < dequeued; ++i) {
        trail_draw.push_back(buf[i]);
      }
    } while (dequeued);
    if (dequeued) {
      phase = animation::Animating;
    }
    constexpr auto kMaxTrailPoints = 256;
    if (trail_draw.size() > kMaxTrailPoints) {
      trail_draw.erase(trail_draw.begin(),
                       trail_draw.begin() + trail_draw.size() - kMaxTrailPoints);
    }
    return animation::Finished;
  }
  void Draw(SkCanvas& canvas) const override {
    auto bounds = *TextureBounds();
    canvas.save();
    canvas.translate(bounds.left, bounds.bottom);
    float scale = WidgetScale();
    canvas.scale(scale, scale);
    mouse::base_texture.draw(canvas);
    dpad_image.draw(canvas);
    canvas.restore();
    canvas.save();
    SkPath path;
    Vec2 cursor = {0, 0};
    float trail_scale = 1;
    path.moveTo(cursor.x, cursor.y);
    constexpr float kDisplayRadius = 1.6_mm;
    for (auto& delta : std::ranges::reverse_view(trail_draw)) {
      cursor += delta;
      path.lineTo(-cursor.x, cursor.y);
      float cursor_dist = Length(cursor);
      float trail_scale_new = kDisplayRadius / cursor_dist;
      if (trail_scale_new < trail_scale) {
        trail_scale = trail_scale_new;
      }
    }
    canvas.translate(-0.05_mm, -2.65_mm);  // move the trail end to the center of display

    static const auto runtime_effect = []() {
      SkPaint paint;
      Status status_ignore;
      auto runtime_effect =
          resources::CompileShader(embedded::assets_pixel_grid_rt_sksl, status_ignore);
      if (!OK(status_ignore)) {
        FATAL << status_ignore;
      }
      return runtime_effect;
    }();

    canvas.scale(trail_scale, trail_scale);

    auto matrix = canvas.getLocalToDeviceAs3x3();
    SkMatrix inverse;
    (void)matrix.invert(&inverse);

    SkVector dpd[2] = {SkVector(1, 0), SkVector(0, 1)};
    inverse.mapVectors(dpd, 2);
    SkPaint display_paint;
    display_paint.setShader(
        runtime_effect->makeShader(SkData::MakeWithCopy((void*)&dpd, sizeof(dpd)), nullptr, 0));

    canvas.drawCircle(0, 0, kDisplayRadius / trail_scale, display_paint);
    SkPaint trail_paint;
    trail_paint.setColor("#CCCCCC"_color);
    trail_paint.setStyle(SkPaint::kStroke_Style);
    if (dpd[0].x() < 1) {
      trail_paint.setStrokeWidth(1);
    }
    canvas.drawPath(path, trail_paint);
    canvas.restore();
  }

  // Mouse Move is supposed to be much smaller than regular mouse widget.
  //
  // This function returns the proper scaling factor.
  static float WidgetScale() {
    float texture_height = mouse::base_texture.height();
    float desired_height = 1.2_cm;
    return desired_height / texture_height;
  }

  Optional<Rect> TextureBounds() const override {
    float scale = WidgetScale();
    float width = mouse::base_texture.width();
    float height = mouse::base_texture.height();
    Rect bounds =
        Rect(-width / 2 * scale, -height / 2 * scale, width / 2 * scale, height / 2 * scale);
    return bounds;
  }

  void ConnectionPositions(Vec<Vec2AndDir>& out_positions) const override {
    Rect bounds = *TextureBounds();
    out_positions.push_back(Vec2AndDir{.pos = bounds.TopCenter(), .dir = -90_deg});
    out_positions.push_back(Vec2AndDir{.pos = bounds.LeftCenter(), .dir = 0_deg});
    out_positions.push_back(Vec2AndDir{.pos = bounds.RightCenter(), .dir = 180_deg});
  }
};

Ptr<gui::Widget> MouseMove::MakeWidget() { return MakePtr<MouseMoveWidget>(AcquireWeakPtr()); }

Vec2 mouse_move_accumulator;

void MouseMove::OnMouseMove(Vec2 vec) {
  mouse_move_accumulator += vec;
  vec = Vec2(truncf(mouse_move_accumulator.x), truncf(mouse_move_accumulator.y));
  mouse_move_accumulator -= vec;
#if defined(__linux__)
  if (vec.x != 0 || vec.y != 0) {
    xcb_test_fake_input(xcb::connection, XCB_MOTION_NOTIFY, true, XCB_CURRENT_TIME, XCB_WINDOW_NONE,
                        vec.x, vec.y, 0);
    xcb::flush();
  }
#endif
  // TODO: visualize the mouse_move_accumulator
  ForEachWidget([vec](gui::RootWidget& root, gui::Widget& widget) {
    MouseMoveWidget& mouse_move_widget = static_cast<MouseMoveWidget&>(widget);
    mouse_move_widget.trail.try_enqueue(mouse_move_widget.trail_token, vec);
    widget.WakeAnimation();
  });
}

}  // namespace automat::library