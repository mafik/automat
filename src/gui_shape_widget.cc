#include "gui_shape_widget.h"

#include <include/utils/SkParsePath.h>

#include "log.h"

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
  SkPath path;
  if (!SkParsePath::FromSVGString(svg_path, &path)) {
    LOG() << "Failed to parse SVG path: " << svg_path;
  }
  constexpr float kScale = 0.0254f / 96;
  path = path.makeScale(kScale, kScale);
  SkPaint paint;
  paint.setAntiAlias(true);
  paint.setColor(fill_color);
  return std::make_unique<ShapeWidget>(path, paint);
}

} // namespace automaton::gui