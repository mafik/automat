#pragma once

#include <include/core/SkCanvas.h>
#include <include/core/SkFont.h>
#include <include/core/SkFontMetrics.h>

#include <cmath>
#include <string_view>

namespace automat::gui {

struct Font {
  SkFont sk_font;
  float font_scale;
  float line_thickness;

  static std::unique_ptr<Font> Make(float letter_size_mm, float weight = 400);

  // TODO: If this causes performance issues, cache text shaping / SkTextBlob
  // results somehow
  void DrawText(SkCanvas& canvas, std::string_view text, const SkPaint& paint);
  float MeasureText(std::string_view text);
  float PositionFromIndex(std::string_view text, int index);
  int IndexFromPosition(std::string_view text, float x);
  // TODO: If this causes performance issues, use ICU directly rather than going
  // through SkShaper
  int PrevIndex(std::string_view text, int index);
  int NextIndex(std::string_view text, int index);
};

Font& GetFont();

}  // namespace automat::gui