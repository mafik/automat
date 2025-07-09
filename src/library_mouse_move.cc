// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#include "library_mouse_move.hh"

#include <include/core/SkBlendMode.h>

#include "xcb/xproto.h"

#if defined(__linux__)
#include <xcb/xtest.h>
#endif

#include "embedded.hh"
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

constexpr float kTurtleHeight = 1.2_cm;

PersistentImage turtle = PersistentImage::MakeFromAsset(
    embedded::assets_turtle_webp, PersistentImage::MakeArgs{.height = kTurtleHeight});

PersistentImage pointer_image = PersistentImage::MakeFromAsset(
    embedded::assets_pointer_webp,
    PersistentImage::MakeArgs{.height = kTurtleHeight / 2,
                              .sampling_options = kNearestMipmapSamplingOptions});

// A turtle with a pixelated cursor on its back
struct MouseMoveWidget : Object::FallbackWidget {
  MouseMoveWidget(WeakPtr<MouseMove>&& weak_mouse_move) { object = std::move(weak_mouse_move); }
  SkPath Shape() const override {
    float turtle_width = turtle.width();
    Rect bounds = Rect(-turtle_width / 2, -kTurtleHeight / 2, turtle_width / 2, kTurtleHeight / 2);
    return SkPath::Oval(bounds);
  }
  void Draw(SkCanvas& canvas) const override {
    canvas.save();
    canvas.translate(-turtle.width() / 2, -kTurtleHeight / 2);
    turtle.draw(canvas);
    canvas.restore();

    canvas.save();
    canvas.translate(-pointer_image.width() / 2, -pointer_image.height() / 2 - 1_mm);
    pointer_image.paint.setColorFilter(SkColorFilters::Lighting("#A0A0A0"_color, "#303030"_color));
    pointer_image.paint.setBlendMode(SkBlendMode::kColorDodge);
    pointer_image.draw(canvas);
    pointer_image.paint.setBlendMode(SkBlendMode::kMultiply);
    pointer_image.draw(canvas);
    canvas.restore();
  }

  void ConnectionPositions(Vec<Vec2AndDir>& out_positions) const override {
    float turtle_width = turtle.width();
    Rect bounds = Rect(-turtle_width / 2, -kTurtleHeight / 2, turtle_width / 2, kTurtleHeight / 2);
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
}

}  // namespace automat::library