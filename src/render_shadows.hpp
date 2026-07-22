#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include <include/core/SkCanvas.h>
#include <include/core/SkImage.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkSpan.h>
#include <include/gpu/graphite/Recorder.h>

#include "math.hpp"

namespace automat {

// One textured widget that casts a drop shadow onto whatever its baker composites beneath it.
struct ShadowCaster {
  sk_sp<SkImage> texture;
  SkIRect surface_bounds_root;
  SkMatrix matrix;  // widget local -> window px
  Rect surface_bounds_local;
  float elevation;  // metres
};

inline Rect ShadowBounds(const Rect& caster_bounds, float elevation) {
  return caster_bounds.Outset(1.35f * elevation);
}

// Call on the recorder about to record the baker's replay; DrawShadow quads emitted during that
// replay shade each pixel by the height difference between the caster and this map.
void RenderShadowHeightMap(skgpu::graphite::Recorder&, SkSpan<const ShadowCaster>,
                           SkISize baker_size, SkIVector baker_origin);

void DrawShadow(SkCanvas&, const ShadowCaster&);

void ShutdownShadows(skgpu::graphite::Recorder&);

}  // namespace automat
