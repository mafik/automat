// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "gui_shape_widget.hh"

#include "svg.hh"

namespace automat::gui {

ShapeWidget::ShapeWidget(SkPath path) : path(path) {}

SkPath ShapeWidget::Shape(animation::Display*) const { return path; }

animation::Phase ShapeWidget::Draw(DrawContext& ctx) const {
  ctx.canvas.drawPath(path, paint);
  return animation::Finished;
}

std::unique_ptr<Widget> MakeShapeWidget(const char* svg_path, SkColor fill_color,
                                        const SkMatrix* transform) {
  SkPath path = PathFromSVG(svg_path);
  if (transform) {
    path.transform(*transform);
  }
  auto ret = std::make_unique<ShapeWidget>(path);
  ret->paint.setAntiAlias(true);
  ret->paint.setColor(fill_color);
  return ret;
}

}  // namespace automat::gui