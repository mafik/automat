#include "gui_text.hh"

#include "font.hh"
#include "gui_constants.hh"

namespace automat::gui {

Text::Text(std::string_view text) : text(text){};

SkPath Text::Shape(animation::Display*) const {
  float w = GetFont().MeasureText(text);
  return SkPath::Rect(SkRect::MakeWH(w, kLetterSize));
}

void Text::Draw(DrawContext& ctx) const { GetFont().DrawText(ctx.canvas, text, paint); }

}  // namespace automat::gui