#pragma once

#include <include/core/SkPaint.h>
#include <include/core/SkPath.h>

#include "widget.h"

namespace automaton::gui {

struct ShapeWidget : Widget {
  SkPath path;
  SkPaint paint;

  ShapeWidget(SkPath path, SkPaint paint);
  SkPath Shape() const override;
  void Draw(SkCanvas &, animation::State &) const override;
};

std::unique_ptr<Widget> MakeShapeWidget(const char *svg_path,
                                        SkColor fill_color);

} // namespace automaton::gui