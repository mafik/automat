#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include <include/core/SkCanvas.h>

namespace automat::ui {

// Return a paint that can be used to draw things with a shadow.
//
// Direction is based on SkCanvas CTM.
//
// Size of the shadow is bounded by elevation_mm * 2;
SkPaint ShadowPaint(SkCanvas&, float elevation_mm);

}  // namespace automat::ui