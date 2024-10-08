// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <include/core/SkDrawable.h>
#include <include/core/SkPaint.h>
#include <include/core/SkRect.h>

namespace automat {

struct Drawable {
  std::unique_ptr<SkDrawable> sk;
  Drawable();
  void draw(SkCanvas*, SkScalar x, SkScalar y);

  virtual SkRect onGetBounds() = 0;
  virtual void onDraw(SkCanvas*) = 0;
};

struct PaintDrawable : Drawable {
  SkPaint paint;
};

}  // namespace automat