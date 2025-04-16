// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <include/core/SkCanvas.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPath.h>

#include "font.hh"
#include "str.hh"

namespace automat {

// A minimal alternative to SkDrawable, for internal use in Automat.
// Can be used to represent arbitrary object that can be drawn on a canvas.
struct Drawable {
  virtual ~Drawable() = default;
  virtual void onDraw(SkCanvas*) = 0;
};

// A base class for Drawables that may be drawn with arbitrary SkPaint.
struct PaintDrawable : Drawable {
  SkPaint paint;
};

struct TextDrawable : PaintDrawable {
  maf::Str text;
  float width;
  float letter_size;
  gui::Font& font;
  TextDrawable(maf::StrView text, float letter_size, gui::Font& font);

  void onDraw(SkCanvas* canvas) override;
};

struct DrawableSkPath : PaintDrawable {
  SkPath path;
  DrawableSkPath(SkPath path) : path(std::move(path)) {}
  void onDraw(SkCanvas* c) override { c->drawPath(path, paint); }
};

}  // namespace automat