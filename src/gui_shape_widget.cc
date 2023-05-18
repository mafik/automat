#include "gui_shape_widget.h"

#include "svg.h"

namespace automaton::gui {

ShapeWidget::ShapeWidget(SkPath path, SkPaint paint)
    : path(path), paint(paint) {}

SkPath ShapeWidget::Shape() const { return path; }

void ShapeWidget::Draw(SkCanvas &canvas,
                       animation::State &animation_state) const {
  DrawColored(canvas, animation_state, paint);
}

void ShapeWidget::DrawColored(SkCanvas &canvas, animation::State &,
                              const SkPaint &paint_arg) const {
  canvas.drawPath(path, paint_arg);
}

std::unique_ptr<Widget> MakeShapeWidget(const char *svg_path,
                                        SkColor fill_color) {
  SkPath path = PathFromSVG(svg_path);
  SkPaint paint;
  paint.setAntiAlias(true);
  paint.setColor(fill_color);
  return std::make_unique<ShapeWidget>(path, paint);
}

} // namespace automaton::gui