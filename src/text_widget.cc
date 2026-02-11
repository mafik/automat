// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include "text_widget.hh"

#include <include/core/SkBlurTypes.h>

#include "font.hh"
#include "units.hh"

namespace automat {

std::unique_ptr<ui::Font> kHelsinkiFont = ui::Font::MakeV2(ui::Font::GetHelsinki(), 3_mm);

TextWidget::TextWidget(ui::Widget* parent, Str text)
    : ui::Widget(parent), width(kHelsinkiFont->MeasureText(text)), text(text) {}
Optional<Rect> TextWidget::TextureBounds() const {
  return Rect(0, -kHelsinkiFont->descent, width, -kHelsinkiFont->ascent);
}
SkPath TextWidget::Shape() const { return SkPath::Rect(*TextureBounds()); }
void TextWidget::Draw(SkCanvas& canvas) const {
  if constexpr (false) {  // outline
    SkPaint outline;
    outline.setColor(SK_ColorBLACK);
    outline.setStyle(SkPaint::kStroke_Style);
    outline.setStrokeWidth(1_mm / kHelsinkiFont->font_scale);
    kHelsinkiFont->DrawText(canvas, text, outline);
  }
  if constexpr (false) {  // shadow
    canvas.save();
    SkPaint shadow;
    shadow.setColor(SK_ColorBLACK);
    shadow.setMaskFilter(SkMaskFilter::MakeBlur(SkBlurStyle::kNormal_SkBlurStyle,
                                                0.5_mm / kHelsinkiFont->font_scale));
    canvas.translate(0, -0.5_mm);
    kHelsinkiFont->DrawText(canvas, text, shadow);
    canvas.restore();
  }
  SkPaint paint;
  paint.setColor(SK_ColorWHITE);
  kHelsinkiFont->DrawText(canvas, text, paint);
}
}  // namespace automat