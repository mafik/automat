#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include <include/core/SkCanvas.h>
#include <include/core/SkImage.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkShader.h>

#include "math.hpp"
#include "units.hpp"

namespace automat {

constexpr float kMaxShadowHeight = 16_mm;

inline Rect ShadowBounds(const Rect& caster_bounds, float elevation) {
  return caster_bounds.Outset(1.35f * elevation);
}

void DrawShadow(SkCanvas&, sk_sp<SkShader> silhouette, sk_sp<SkImage> height_map,
                const SkMatrix& local_to_height, float elevation, float alpha, const Rect& bounds);

}  // namespace automat
