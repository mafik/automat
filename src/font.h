#pragma once

#include <string_view>

#include <include/core/SkCanvas.h>
#include <include/core/SkFont.h>
#include <include/core/SkFontMetrics.h>

namespace automaton::gui {

constexpr float kLetterSizeMM = 2;
constexpr float kLetterSize = kLetterSizeMM / 1000;

struct Font {
  SkFont sk_font;
  float font_scale;
  float line_thickness;

  static std::unique_ptr<Font> Make(float letter_size_mm);

  void DrawText(SkCanvas &canvas, std::string_view text, SkPaint &paint);
  float MeasureText(std::string_view text);
  float PositionFromIndex(std::string_view text, int index);
  int IndexFromPosition(std::string_view text, float x);
};

Font &GetFont();

} // namespace automaton::gui