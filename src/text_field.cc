#include "text_field.h"

#include <include/core/SkColor.h>

#include "font.h"

namespace automaton::gui {

void TextField::OnHover(bool hover, dual_ptr_holder& animation_state) {
  has_hover[animation_state] = hover;
}

void TextField::OnFocus(bool focus, dual_ptr_holder& animation_state) {
  has_focus = focus;
}

void TextField::Draw(SkCanvas &canvas, dual_ptr_holder& animation_state) {  
  Font& font = GetFont();
  SkRect rect = SkRect::MakeXYWH(0, 0, width, kTextFieldHeight);
  SkColor c_inactive_bg = SkColorSetRGB(0xe0, 0xe0, 0xe0);
  SkColor c_fg = SK_ColorBLACK;
  SkPaint paint_bg;
  paint_bg.setColor(c_inactive_bg);
  canvas.drawPath(GetShape(), paint_bg);
  canvas.translate(kTextMargin, (kTextFieldHeight - gui::kLetterSize) / 2);
  SkPaint underline;
  underline.setColor(c_fg);
  SkRect underline_rect = SkRect::MakeXYWH(0, -font.line_thickness, width - 2 * kTextMargin, - font.line_thickness);
  canvas.drawRect(underline_rect, underline);
  SkPaint text_fg;
  text_fg.setColor(c_fg);
  if (text) {
    font.DrawText(canvas, *text, text_fg);
  }
}

SkPath TextField::GetShape() {
  SkRect bounds = SkRect::MakeXYWH(0, 0, width, kTextFieldHeight);
  return SkPath::RRect(bounds, kTextMargin, kTextMargin);
}

std::unique_ptr<Action> TextField::KeyDown(Key) {
  
}

}