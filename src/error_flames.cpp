// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include "error_flames.hpp"

#include <include/core/SkBlurTypes.h>
#include <include/core/SkMaskFilter.h>
#include <include/core/SkPaint.h>
#include <include/effects/SkImageFilters.h>
#include <include/effects/SkRuntimeEffect.h>

#include "embedded.hpp"
#include "font.hpp"
#include "global_resources.hpp"
#include "log.hpp"
#include "status.hpp"
#include "ui_constants.hpp"
#include "units.hpp"

namespace automat {

constexpr float kBlurRadius = 7_mm;
constexpr float kReach = 3 * kBlurRadius;
constexpr float kSpeed = 3;

static Vec2 TextOrigin(const Rect& shape_bounds) {
  return Vec2(shape_bounds.left - 0.25_mm, shape_bounds.bottom - 0.75_mm - ui::kLetterSize * 1.5);
}

void ErrorFlames::SetText(StrView new_text) {
  if (text == new_text) return;
  text = new_text;
  text_width = ui::GetFont().MeasureText(text);
}

SkPath ErrorFlames::Shape() const { return SkPath(); }

Optional<Rect> ErrorFlames::DrawBounds() const {
  if (!parent) return std::nullopt;
  Rect bounds = parent->shape.getBounds();
  if (bounds.sk.isEmpty()) return std::nullopt;
  Rect draw_bounds = bounds.Outset(kReach);
  if (!text.empty()) {
    Vec2 origin = TextOrigin(bounds);
    draw_bounds.ExpandToInclude(
        Rect(SkRect::MakeLTRB(origin.x, origin.y - ui::kLetterSize, origin.x + text_width,
                              origin.y + 2 * ui::kLetterSize)));
  }
  return draw_bounds;
}

ui::Tock ErrorFlames::Tick(time::Timer& timer) {
  phase += timer.d * kSpeed;
  Tock tock = Tock::Drawing;
  uint32_t genid = parent ? parent->shape.getGenerationID() : 0;
  if (genid != parent_shape_genid) {
    parent_shape_genid = genid;
    tock.shape = true;
  }
  return tock;
}

void ErrorFlames::Draw(SkCanvas& canvas) const {
  if (!parent) return;
  SkMatrix local_to_device = canvas.getLocalToDeviceAs3x3();
  SkPath shape_px = parent->shape.makeTransform(local_to_device);
  Rect bounds_px = shape_px.getBounds();
  float blur_radius = local_to_device.mapRadius(kBlurRadius);
  Rect clip_px = bounds_px.Outset(blur_radius * 3);

  Status status;
  static auto effect = resources::CompileShader(embedded::assets_error_sksl, status);
  if (!effect) {
    ERROR_ONCE << "error.sksl: " << status;
    return;
  }
  SkRuntimeEffectBuilder builder(effect);
  builder.uniform("iTime") = phase;
  builder.uniform("iLeft") = bounds_px.left;
  builder.uniform("iRight") = bounds_px.right;
  builder.uniform("iTop") = bounds_px.top;
  builder.uniform("iBottom") = bounds_px.bottom;
  SkPaint fire_paint;
  fire_paint.setImageFilter(SkImageFilters::RuntimeShader(builder, "iMask", nullptr));

  // Note that we're saving the canvas state twice.
  // This is because of https://issues.skia.org/issues/447458443
  // Otherwise the coordinates passed to the shader will be messed up.
  canvas.save();
  canvas.resetMatrix();
  canvas.clipRect(clip_px.sk);
  canvas.saveLayer(&clip_px.sk, &fire_paint);

  SkPaint stroke_paint;
  stroke_paint.setStyle(SkPaint::kStroke_Style);
  stroke_paint.setStrokeWidth(1);
  canvas.drawPath(shape_px, stroke_paint);

  SkPaint paint;
  paint.setMaskFilter(SkMaskFilter::MakeBlur(kOuter_SkBlurStyle, blur_radius, false));
  canvas.drawPath(shape_px, paint);
  paint.setMaskFilter(SkMaskFilter::MakeBlur(kOuter_SkBlurStyle, blur_radius / 4, false));
  canvas.drawPath(shape_px.makeToggleInverseFillType(), paint);

  canvas.restore();
  canvas.restore();

  if (!text.empty()) {
    SkPaint text_paint;
    text_paint.setColor(SK_ColorRED);
    text_paint.setAntiAlias(true);
    canvas.save();
    Vec2 origin = TextOrigin(parent->shape.getBounds());
    canvas.translate(origin.x, origin.y);
    ui::GetFont().DrawText(canvas, text, text_paint);
    canvas.restore();
  }
}

}  // namespace automat
