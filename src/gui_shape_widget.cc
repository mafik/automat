#include "gui_shape_widget.h"

#include <include/utils/SkParsePath.h>

#include "log.h"

namespace automaton::gui {

ShapeWidget::ShapeWidget(SkPath path, SkPaint paint)
    : path(path), paint(paint) {}

SkPath ShapeWidget::Shape() const { return path; }

void ShapeWidget::Draw(SkCanvas &canvas, animation::State &) const {
  canvas.drawPath(path, paint);
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