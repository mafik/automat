#include "text_field.h"

#include <include/core/SkColor.h>

#include "font.h"
#include "log.h"

namespace automaton::gui {

void TextField::PointerOver(Pointer &pointer, animation::State &state) {
  pointer.PushIcon(Pointer::kIconIBeam);
  hover_ptr[state].Increment();
}

void TextField::PointerLeave(Pointer &pointer, animation::State &state) {
  pointer.PopIcon();
  hover_ptr[state].Decrement();
}

void TextField::OnFocus(bool focus, animation::State &animation_state) {
  has_focus = focus;
}

void TextField::Draw(SkCanvas &canvas,
                     animation::State &animation_state) const {
  auto &hover = hover_ptr[animation_state].animation;
  hover.Tick(animation_state);

  Font &font = GetFont();
  SkPath shape = Shape();
  SkColor c_inactive_bg = SkColorSetRGB(0xe0, 0xe0, 0xe0);
  SkColor c_fg = SK_ColorBLACK;
  SkPaint paint_bg;
  paint_bg.setColor(c_inactive_bg);
  canvas.drawPath(shape, paint_bg);
  if (hover.value > 0.0001) {
    SkPaint hover_outline;
    hover_outline.setColor(SkColorSetRGB(0xff, 0x00, 0x00));
    hover_outline.setStyle(SkPaint::kStroke_Style);
    hover_outline.setStrokeWidth(hover.value * 0.0005);
    canvas.drawPath(shape, hover_outline);
  }
  canvas.translate(kTextMargin, (kTextFieldHeight - gui::kLetterSize) / 2);
  SkPaint underline;
  underline.setColor(c_fg);
  SkRect underline_rect = SkRect::MakeXYWH(
      0, -font.line_thickness, width - 2 * kTextMargin, -font.line_thickness);
  canvas.drawRect(underline_rect, underline);
  SkPaint text_fg;
  text_fg.setColor(c_fg);
  if (text) {
    font.DrawText(canvas, *text, text_fg);
  }
}

SkPath TextField::Shape() const {
  SkRect bounds = SkRect::MakeXYWH(0, 0, width, kTextFieldHeight);
  return SkPath::RRect(bounds, kTextMargin, kTextMargin);
}

std::unique_ptr<Action> TextField::KeyDownAction(Key) { return nullptr; }

} // namespace automaton::gui