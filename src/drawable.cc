// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "drawable.hh"

namespace automat {

TextDrawable::TextDrawable(StrView text, float letter_size, gui::Font& font)
    : text(text), letter_size(letter_size), font(font) {
  width = font.MeasureText(text);
}

void TextDrawable::onDraw(SkCanvas* canvas) {
  canvas->translate(-width / 2, -letter_size / 2);
  font.DrawText(*canvas, text, paint);
}

}  // namespace automat