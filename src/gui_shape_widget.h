#pragma once

#include <include/core/SkPaint.h>
#include <include/core/SkPath.h>

#include "widget.h"

namespace automat::gui {

struct ShapeWidget : Widget, FillMixin {
  SkPath path;

  ShapeWidget(SkPath path);
  SkPath Shape() const override;
  void Draw(DrawContext&) const override;
};

std::unique_ptr<Widget> MakeShapeWidget(const char* svg_path, SkColor fill_color);

}  // namespace automat::gui