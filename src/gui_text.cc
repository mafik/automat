#include "gui_text.h"

#include "font.h"

namespace automat::gui {

Text::Text(std::string_view text) : text(text){};

SkPath Text::Shape() const {
  float w = GetFont().MeasureText(text);
  return SkPath::Rect(SkRect::MakeWH(w, kLetterSize));
}

void Text::Draw(DrawContext& ctx) const {
  SkPaint paint;
  GetFont().DrawText(ctx.canvas, text, paint);
}

}  // namespace automat::gui