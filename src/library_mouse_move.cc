// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include "library_mouse_move.hh"

#include <include/core/SkBlendMode.h>

#include <atomic>

#if defined(__linux__)
#include <xcb/xproto.h>
#include <xcb/xtest.h>
#endif

#include "embedded.hh"
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

Ptr<Object> MouseMove::Clone() const { return MAKE_PTR(MouseMove); }

PersistentImage dpad_image = PersistentImage::MakeFromAsset(
    embedded::assets_mouse_dpad_webp, PersistentImage::MakeArgs{.scale = mouse::kTextureScale});

// A turtle with a pixelated cursor on its back
struct MouseMoveWidget : Object::FallbackWidget {
  constexpr static size_t kMaxTrailPoints = 256;
  std::atomic<int> trail_end_idx = 0;
  std::atomic<Vec2> trail[kMaxTrailPoints] = {};

  MouseMoveWidget(gui::Widget& parent, WeakPtr<MouseMove>&& weak_mouse_move)
      : FallbackWidget(parent) {
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
    constexpr float kDisplayRadius = 1.6_mm;
    float trail_scale =
        kDisplayRadius / 15;  // initial scale shows at least 15 pixels (0 and 10 pixel axes)
    path.moveTo(cursor.x, cursor.y);
    int end = trail_end_idx.load(std::memory_order_relaxed);
    for (int i = end + kMaxTrailPoints - 1; i != end; --i) {
      Vec2 delta = trail[i % kMaxTrailPoints].load(std::memory_order_relaxed);
      cursor += delta;
      path.lineTo(-cursor.x, cursor.y);
      float cursor_dist = Length(cursor);
      float trail_scale_new = kDisplayRadius / cursor_dist;
      if (trail_scale_new < trail_scale) {
        trail_scale = trail_scale_new;
      }
    }
    canvas.translate(-0.05_mm, -2.65_mm);  // move the trail end to the center of display

    canvas.scale(trail_scale, trail_scale);

    auto matrix = canvas.getLocalToDeviceAs3x3();
    SkMatrix inverse;
    (void)matrix.invert(&inverse);

    SkVector dpd[2] = {SkVector(1, 0), SkVector(0, 1)};
    inverse.mapVectors(dpd);
    SkPaint display_paint;
    display_paint.setShader(mouse::GetPixelGridRuntimeEffect().makeShader(
        SkData::MakeWithCopy((void*)&dpd, sizeof(dpd)), nullptr, 0));

    canvas.drawCircle(0, 0, kDisplayRadius / trail_scale, display_paint);
    SkPaint trail_paint;
    trail_paint.setColor("#CCCCCC"_color);
    trail_paint.setStyle(SkPaint::kStroke_Style);
    if (dpd[0].x() < 1) {
      trail_paint.setStrokeWidth(1);
      trail_paint.setStrokeCap(SkPaint::kSquare_Cap);
      trail_paint.setStrokeJoin(SkPaint::kMiter_Join);
      trail_paint.setStrokeMiter(2);
    }
    canvas.drawPath(path, trail_paint);
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

std::unique_ptr<gui::Widget> MouseMove::MakeWidget(gui::Widget& parent) {
  return std::make_unique<MouseMoveWidget>(parent, AcquireWeakPtr());
}

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
  ForEachWidget([vec](gui::RootWidget& root, gui::Widget& widget) {
    MouseMoveWidget& mouse_move_widget = static_cast<MouseMoveWidget&>(widget);
    int new_start = mouse_move_widget.trail_end_idx.fetch_add(1, std::memory_order_relaxed);
    int i = (new_start + MouseMoveWidget::kMaxTrailPoints - 1) % MouseMoveWidget::kMaxTrailPoints;
    mouse_move_widget.trail[i].store(vec, std::memory_order_relaxed);
    widget.WakeAnimation();
  });
}

}  // namespace automat::library