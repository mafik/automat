#include "loading_animation.h"

#include "log.h"
#include "win.h"

#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/core/SkPath.h>
#include <include/effects/SkRuntimeEffect.h>
#include <src/core/SkRuntimeEffectPriv.h>

namespace automaton {

HypnoRect anim;

void LoadingAnimation::OnPaint(SkCanvas &canvas,
                               std::function<void(SkCanvas &)> paint) {
  last = now;
  now = std::chrono::system_clock::now();
  t = now - start;
  dt = now - last;
  PrePaint(canvas);
  paint(canvas);
  PostPaint(canvas);
}

SkColor SkColorFromLittleEndian(uint32_t little_endian_rgb) {
  return SkColorSetRGB((little_endian_rgb >> 16) & 0xff,
                       (little_endian_rgb >> 8) & 0xff,
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
    LOG() << err.c_str();
  }
  shader_builder.reset(new SkRuntimeShaderBuilder(effect));
  shader_builder->uniform("top_color") = SkColor4f::FromColor(kTopColor);
  shader_builder->uniform("bottom_color") = SkColor4f::FromColor(kBottomColor);
}

float HypnoRect::Twist(SkCanvas &canvas, float factor) {
  float scale = pow(kScalePerTwist, unfold * factor);
  canvas.rotate(kDegreesPerTwist * unfold * factor);
  canvas.scale(scale, scale);
  return scale;
}

void HypnoRect::PrePaint(SkCanvas &canvas) {
  if (state == kDone) {
    return;
  }

  shader_builder->uniform("resolution") = SkV2(window_width, window_height);
  paint.setShader(shader_builder->makeShader());

  canvas.clear(kBackgroundColor);

  int cx = window_width / 2;
  int cy = window_height / 2;

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
    base_twist_v += 0.0005;
    base_twist_v *= 1.002;
    base_twist += base_twist_v;
    Twist(canvas, base_twist);
    clip_path.transform(canvas.getTotalMatrix());
    canvas.drawRect(rect, paint);
    canvas.restore();
  } else {
    clip_path.transform(canvas.getTotalMatrix());
    canvas.drawRect(rect, paint);
  }

  unfold += (1 - unfold) * 0.015;

  if (state >= kLoading) {
    first_twist_v += (0.02 - first_twist_v) * 0.01;
  }
  first_twist += first_twist_v;
  if (first_twist > 1) {
    first_twist -= 1;
  }

  Twist(canvas, first_twist);
  if (first_twist > base_twist) {
    canvas.drawRect(rect, paint);
  }

  float window_diag =
      sqrt(window_width * window_width + window_height * window_height);

  for (int i = 0; i < 25; ++i) {
    canvas.save();
    float scale = Twist(canvas, i);
    if (i > base_twist) {
      canvas.drawRect(rect, paint);
    }
    canvas.restore();
    if (rect_side * scale * base_scale > window_diag) {
      if (state == kPostLoading && base_twist > i) {
        state = kDone;
      }
      if (state == kPreLoading) {
        state = kLoading;
      }
      break;
    }
  }

  canvas.restore();
  SkRect bounds = clip_path.computeTightBounds();
  canvas.saveLayer(&bounds, nullptr);
  canvas.clipPath(clip_path);
}

void HypnoRect::PostPaint(SkCanvas &canvas) { canvas.restore(); }

} // namespace automaton