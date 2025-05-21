// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "loading_animation.hh"

#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkPath.h>
#include <include/core/SkTileMode.h>
#include <include/effects/SkGradientShader.h>

#include "color.hh"
#include "random.hh"
#include "root_widget.hh"
#include "time.hh"

using namespace maf;

namespace automat {

HypnoRect anim;

HypnoRect::HypnoRect() {
  paint.setColor(SK_ColorBLACK);
  paint.setStroke(true);
  paint.setAntiAlias(true);
  paint.setStrokeWidth(0.8_mm);

  auto rng = XorShift32::MakeFromCurrentTime();

  float hue_primary = rng.RollFloat(0, 360);
  float hue_secondary = fmodf(hue_primary + 120, 360);
  float hue_bg = fmodf(hue_primary + 60, 360);
  float lightness_fg = rng.RollFloat(0, 100);
  float lightness_bg = fmodf(lightness_fg + 50, 100);
  float fg_sat = lightness_fg > lightness_bg ? 80 : 10;
  float bg_sat = lightness_fg > lightness_bg ? 10 : 80;
  top_color = color::HSLuv(hue_primary, fg_sat, lightness_fg);
  bottom_color = color::HSLuv(hue_secondary, fg_sat, lightness_fg);
  background_color = color::HSLuv(hue_bg, bg_sat, lightness_bg);
}

static float Twist(HypnoRect& a, SkMatrix& transform, float factor) {
  float scale = pow(HypnoRect::kScalePerTwist, a.unfold * factor);
  transform.preRotate(HypnoRect::kDegreesPerTwist * a.unfold * factor);
  transform.preScale(scale, scale);
  return scale;
}

animation::Phase HypnoRect::Tick(time::Timer& timer) {
  t = (time::SteadyNow() - start).count();

  if (state == kPostLoading) {
    base_twist_v += 0.0005 * timer.d;
    base_twist_v *= exp(timer.d * 5);
    base_twist += base_twist_v;
  }

  unfold += (1 - unfold) * -(expm1f(-timer.d * 2));
  base_scale = 1 + cos(t) * 0.2;

  client_width = gui::root_widget->window->client_width;
  client_height = gui::root_widget->window->client_height;
  float rect_side = rect.Width() - paint.getStrokeWidth();

  float outer_rect_side = rect_side * base_scale * pow(kScalePerTwist, unfold * 25);
  client_diag = sqrt(client_width * client_width + client_height * client_height);
  if (outer_rect_side > client_diag) {
    if (state == kPreLoading) {
      state = kLoading;
    }
    first_twist_v += (2 - first_twist_v) * -(expm1f(-timer.d));
  }
  first_twist += first_twist_v * timer.d;
  if (first_twist > 1) {
    first_twist -= 1;
  }

  if (state == kPostLoading && base_twist > 25) {
    state = kDone;
  }
  return LoadingAnimation::Tick(timer);
}

void HypnoRect::PreDraw(SkCanvas& canvas) {
  if (state == kDone) {
    return;
  }

  {
    SkPoint pts[2] = {
        rect.TopCenter(),
        rect.BottomCenter(),
    };
    SkColor colors[2] = {top_color, bottom_color};
    auto matrix = canvas.getLocalToDeviceAs3x3();
    paint.setShader(
        SkGradientShader::MakeLinear(pts, colors, nullptr, 2, SkTileMode::kClamp, 0, nullptr));
  }

  canvas.clear(background_color);

  float rect_side = rect.Width() - paint.getStrokeWidth();

  SkMatrix base_transform;
  float base_rotation = -t * 10;
  base_transform.preTranslate(gui::root_widget->size.width / 2, gui::root_widget->size.height / 2);
  base_transform.preRotate(base_rotation);
  base_transform.preScale(base_scale, base_scale);

  SkMatrix clip_transform = base_transform;

  canvas.save();
  canvas.concat(base_transform);

  if (state == kPostLoading) {
    SkMatrix transform;
    Twist(*this, transform, base_twist);
    canvas.save();
    canvas.concat(transform);
    clip_transform.preConcat(transform);
    canvas.drawRect(rect, paint);
    canvas.restore();
  } else {
    canvas.drawRect(rect, paint);
  }

  SkPath clip_path = SkPath::Rect(rect);
  clip_path.transform(clip_transform);

  SkMatrix transform2;
  Twist(*this, transform2, first_twist);
  canvas.concat(transform2);
  if (first_twist > base_twist) {
    canvas.drawRect(rect, paint);
  }

  for (int i = 0; i < 25; ++i) {
    canvas.save();
    SkMatrix transform;
    float twist_scale = Twist(*this, transform, i);
    canvas.concat(transform);
    if (i > base_twist) {
      canvas.drawRect(rect, paint);
    }
    canvas.restore();
    float rect_side_scaled = rect_side * base_scale * twist_scale;
    if (rect_side_scaled > client_diag) {
      break;
    }
  }

  canvas.restore();
  SkRect bounds = clip_path.computeTightBounds();
  canvas.saveLayer(nullptr, nullptr);
  canvas.clipPath(clip_path);
}

void HypnoRect::PostDraw(SkCanvas& canvas) { canvas.restore(); }

}  // namespace automat