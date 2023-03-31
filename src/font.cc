#include "font.h"

namespace automaton::gui {

std::unique_ptr<Font> Font::Make(float letter_size_mm) {
  constexpr float kMilimetersPerInch = 25.4;
  constexpr float kPointsPerInch = 72;
  // We want text to be `letter_size_mm` tall (by cap size).
  float letter_size_pt = letter_size_mm / kMilimetersPerInch * kPointsPerInch;
  float font_size_guess =
      letter_size_pt / 0.7f; // this was determined empirically
  // Create the font using the approximate size.
  SkFont sk_font(nullptr, font_size_guess, 1.f, 0.f);
  SkFontMetrics metrics;
  sk_font.getMetrics(&metrics);
  // The `fCapHeight` is the height of the capital letters.
  float font_scale = 0.001 * letter_size_mm / metrics.fCapHeight;
  float line_thickness = metrics.fUnderlineThickness * font_scale;
  return std::make_unique<Font>(sk_font, font_scale, line_thickness);
}

void Font::DrawText(SkCanvas &canvas, std::string_view text, SkPaint &paint) {
  canvas.scale(font_scale, -font_scale);
  canvas.drawSimpleText(text.data(), text.size(), SkTextEncoding::kUTF8, 0, 0,
                        sk_font, paint);
  canvas.scale(1 / font_scale, -1 / font_scale);
}

float Font::MeasureText(std::string_view text) {
  return sk_font.measureText(text.data(), text.size(), SkTextEncoding::kUTF8) *
         font_scale;
}

Font &GetFont() {
  static std::unique_ptr<Font> font = Font::Make(kLetterSizeMM);
  return *font;
}

} // namespace automaton::gui