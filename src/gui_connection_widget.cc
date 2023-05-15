#include "gui_connection_widget.h"

#include <include/core/SkColor.h>

#include "font.h"
#include "gui_constants.h"

namespace automaton::gui {

ConnectionWidget::ConnectionWidget(Location *from, std::string_view label)
    : from(from), label(label) {}

vec2 ConnectionWidget::Center() const {
  SkRect from_bounds = from->Shape().getBounds();
  return Vec2(from->position.X + from_bounds.left() + kRadius,
              from->position.Y + from_bounds.top() - kMargin - kRadius);
}

SkPath ConnectionWidget::Shape() const {
  SkPath path;
  vec2 center = Center();
  path.addCircle(center.X, center.Y, kRadius);
  return path;
}

void ConnectionWidget::Draw(SkCanvas &canvas, animation::State &state) const {
  SkPaint paint;
  paint.setAntiAlias(true);
  paint.setColor(SK_ColorRED);
  SkPath path = Shape();
  canvas.drawPath(path, paint);
  auto &font = GetFont();

  SkRect bounds = path.getBounds();
  vec2 text_pos =
      Vec2(bounds.right() + kMargin, bounds.centerY() - kLetterSize / 2);
  canvas.translate(text_pos.X, text_pos.Y);
  font.DrawText(canvas, label, paint);
  canvas.translate(-text_pos.X, -text_pos.Y);
}

} // namespace automaton::gui