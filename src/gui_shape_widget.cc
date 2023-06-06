#include "gui_shape_widget.h"

#include "svg.h"

namespace automat::gui {

ShapeWidget::ShapeWidget(SkPath path, SkPaint paint) : path(path), paint(paint) {}

SkPath ShapeWidget::Shape() const { return path; }

void ShapeWidget::Draw(DrawContext& ctx) const { DrawColored(ctx, paint); }

void ShapeWidget::DrawColored(DrawContext& ctx, const SkPaint& paint_arg) const {
  ctx.canvas.drawPath(path, paint_arg);
}

std::unique_ptr<Widget> MakeShapeWidget(const char* svg_path, SkColor fill_color) {
  SkPath path = PathFromSVG(svg_path);
  SkPaint paint;
  paint.setAntiAlias(true);
  paint.setColor(fill_color);
  return std::make_unique<ShapeWidget>(path, paint);
}

}  // namespace automat::gui