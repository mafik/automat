// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
#include "render_shadows.hpp"

#include <include/core/SkPaint.h>
#include <include/effects/SkRuntimeEffect.h>

#include "color.hpp"
#include "embedded.hpp"
#include "global_resources.hpp"
#include "log.hpp"
#include "status.hpp"

namespace automat {

void DrawShadow(SkCanvas& canvas, sk_sp<SkShader> silhouette, sk_sp<SkImage> height_map,
                const SkMatrix& local_to_height, float elevation, float alpha,
                const Rect& bounds) {
  if (!silhouette || !height_map) {
    return;
  }
  Status status;
  static auto effect = resources::CompileShader(embedded::assets_shadow_rt_sksl, status);
  if (!effect) {
    ERROR_ONCE << "shadow_rt.sksl: " << status;
    return;
  }
  SkSamplingOptions sampling(SkFilterMode::kLinear);
  SkRuntimeEffectBuilder builder(effect);
  builder.child("caster") = silhouette;
  builder.child("height_map") =
      height_map->makeShader(SkTileMode::kDecal, SkTileMode::kDecal, sampling);
  builder.uniform("local_to_height") = local_to_height;
  builder.uniform("elevation") = elevation;
  auto tint = SkColor4f::FromColor("#09000c5b"_color).premul();
  builder.uniform("tint") = SkV4{tint.fR, tint.fG, tint.fB, tint.fA};
  SkPaint paint;
  paint.setAntiAlias(false);
  paint.setShader(builder.makeShader());
  paint.setAlphaf(alpha);
  canvas.drawRect(bounds.sk, paint);
}

}  // namespace automat
