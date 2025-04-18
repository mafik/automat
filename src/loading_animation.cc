// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "loading_animation.hh"

#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/core/SkPath.h>
#include <include/effects/SkRuntimeEffect.h>
#include <src/core/SkRuntimeEffectPriv.h>

#include "log.hh"
#include "root_widget.hh"

namespace automat {

HypnoRect anim;

void LoadingAnimation::OnPaint(SkCanvas& canvas, std::function<void(SkCanvas&)> paint) {
  last = now;
  now = std::chrono::system_clock::now();
  t = now - start;
  dt = now - last;
  PrePaint(canvas);
  paint(canvas);
  PostPaint(canvas);
}

SkColor SkColorFromLittleEndian(uint32_t little_endian_rgb) {
  return SkColorSetRGB((little_endian_rgb >> 16) & 0xff, (little_endian_rgb >> 8) & 0xff,
                       (little_endian_rgb >> 0) & 0xff);
}

HypnoRect::HypnoRect() {
  paint.setColor(SK_ColorBLACK);
  paint.setStroke(true);
  paint.setAntiAlias(true);
  paint.setStrokeWidth(1);
  SkRuntimeEffect::Options options;
  SkRuntimeEffectPriv::AllowPrivateAccess(&options);
  auto [effect, err] = SkRuntimeEffect::MakeForShader(SkString(sksl), options);
  if (!err.isEmpty()) {
    LOG << err.c_str();
  }
  shader_builder.reset(new SkRuntimeShaderBuilder(effect));
  shader_builder->uniform("top_color") = SkColor4f::FromColor(kTopColor);
  shader_builder->uniform("bottom_color") = SkColor4f::FromColor(kBottomColor);
}

float HypnoRect::Twist(SkCanvas& canvas, float factor) {
  float scale = pow(kScalePerTwist, unfold * factor);
  canvas.rotate(kDegreesPerTwist * unfold * factor);
  canvas.scale(scale, scale);
  return scale;
}

void HypnoRect::PrePaint(SkCanvas& canvas) {
  if (state == kDone) {
    return;
  }

  int client_width = gui::root_widget->window->client_width;
  int client_height = gui::root_widget->window->client_height;

  shader_builder->uniform("resolution") = SkV2(client_width, client_height);
  paint.setShader(shader_builder->makeShader());

  canvas.clear(kBackgroundColor);

  int cx = client_width / 2;
  int cy = client_height / 2;

  SkPath clip_path = SkPath::Rect(rect);
  float rect_side = rect.width() - paint.getStrokeWidth();

  canvas.save();
  canvas.translate(cx, cy);

  float base_rotation = -t.count() * 10;
  canvas.rotate(base_rotation);

  float base_scale = 1 + cos(t.count()) * 0.2;
  canvas.scale(base_scale, base_scale);

  if (state == kPostLoading) {
    canvas.save();
    base_twist_v += 0.0005 * dt.count();
    base_twist_v *= exp(dt.count() * 5);
    base_twist += base_twist_v;
    Twist(canvas, base_twist);
    clip_path.transform(canvas.getTotalMatrix());
    canvas.drawRect(rect, paint);
    canvas.restore();
  } else {
    clip_path.transform(canvas.getTotalMatrix());
    canvas.drawRect(rect, paint);
  }

  unfold += (1 - unfold) * -(expm1f(-dt.count() * 2));

  float outer_rect_side = rect_side * base_scale * pow(kScalePerTwist, unfold * 25);
  float window_diag = sqrt(client_width * client_width + client_height * client_height);
  if (outer_rect_side > window_diag) {
    if (state == kPreLoading) {
      state = kLoading;
    }
    first_twist_v += (2 - first_twist_v) * -(expm1f(-dt.count()));
  }
  first_twist += first_twist_v * dt.count();
  if (first_twist > 1) {
    first_twist -= 1;
  }

  Twist(canvas, first_twist);
  if (first_twist > base_twist) {
    canvas.drawRect(rect, paint);
  }

  for (int i = 0; i < 25; ++i) {
    canvas.save();
    float twist_scale = Twist(canvas, i);
    if (i > base_twist) {
      canvas.drawRect(rect, paint);
    }
    canvas.restore();
    float rect_side_scaled = rect_side * base_scale * twist_scale;
    if (rect_side_scaled > window_diag) {
      break;
    }
  }

  if (state == kPostLoading && base_twist > 25) {
    state = kDone;
  }

  canvas.restore();
  SkRect bounds = clip_path.computeTightBounds();
  canvas.saveLayer(&bounds, nullptr);
  canvas.clipPath(clip_path);
}

void HypnoRect::PostPaint(SkCanvas& canvas) { canvas.restore(); }

}  // namespace automat