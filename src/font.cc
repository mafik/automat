#include "font.h"

#include <modules/skshaper/include/SkShaper.h>
#include <modules/skunicode/include/SkUnicode.h>

#include "log.h"

// TODO: load icudtl.dat from C:\Windows\Globalization\ICU\icudtl.dat
#pragma comment(lib, "skshaper")
#pragma comment(lib, "skunicode")
#pragma comment(lib, "icu")
#pragma comment(lib, "Advapi32")

namespace automaton::gui {

std::unique_ptr<Font> Font::Make(float letter_size_mm) {
  constexpr float kMilimetersPerInch = 25.4;
  constexpr float kPointsPerInch = 72;
  // We want text to be `letter_size_mm` tall (by cap size).
  float letter_size_pt = letter_size_mm / kMilimetersPerInch * kPointsPerInch;
  float font_size_guess =
      letter_size_pt / 0.7f; // this was determined empirically
  // Create the font using the approximate size.
  // TODO: embed & use Noto Color Emoji
  auto typeface =
      SkTypeface::MakeFromName("Segoe UI Emoji", SkFontStyle::Normal());
  SkFont sk_font(typeface, font_size_guess, 1.f, 0.f);
  SkFontMetrics metrics;
  sk_font.getMetrics(&metrics);
  // The `fCapHeight` is the height of the capital letters.
  float font_scale = 0.001 * letter_size_mm / metrics.fCapHeight;
  float line_thickness = metrics.fUnderlineThickness * font_scale;
  return std::make_unique<Font>(sk_font, font_scale, line_thickness);
}

class LineRunHandler : public SkShaper::RunHandler {
public:
  LineRunHandler(const char *utf8Text, SkPoint offset)
      : utf8_text(utf8Text), offset(offset) {}
  sk_sp<SkTextBlob> makeBlob() { return builder.make(); }

  void beginLine() override {
    ascent = 0;
    descent = 0;
    leading = 0;
  }
  void runInfo(const RunInfo &info) override {
    SkFontMetrics metrics;
    info.fFont.getMetrics(&metrics);
    ascent = std::min(ascent, metrics.fAscent);
    descent = std::max(descent, metrics.fDescent);
    leading = std::max(leading, metrics.fLeading);
  }
  void commitRunInfo() override {}
  Buffer runBuffer(const RunInfo &info) override {
    int glyphCount =
        SkTFitsIn<int>(info.glyphCount) ? info.glyphCount : INT_MAX;
    int utf8RangeSize =
        SkTFitsIn<int>(info.utf8Range.size()) ? info.utf8Range.size() : INT_MAX;

    const auto &runBuffer =
        builder.allocRunTextPos(info.fFont, glyphCount, utf8RangeSize);
    if (runBuffer.utf8text && utf8_text) {
      memcpy(runBuffer.utf8text, utf8_text + info.utf8Range.begin(),
             utf8RangeSize);
    }
    clusters = runBuffer.clusters;
    glyph_count = glyphCount;
    cluster_offset = info.utf8Range.begin();

    return {runBuffer.glyphs, // buffer that will be filled with glyph IDs
            runBuffer.points(), nullptr, runBuffer.clusters, offset};
  }
  void commitRunBuffer(const RunInfo &info) override {
    SkASSERT(0 <= cluster_offset);
    for (int i = 0; i < glyph_count; ++i) {
      SkASSERT(clusters[i] >= (unsigned)cluster_offset);
      clusters[i] -= cluster_offset;
    }
    offset += info.fAdvance;
  }
  void commitLine() override {}

  // All values use scaled text units.
  // Scaled text units have flipped Y axis and are significantly larger than
  // meters.

  char const *const utf8_text;
  SkPoint offset; // Position where the letters will be placed (baseline).
  SkTextBlobBuilder builder;

  SkScalar ascent;  // That this is (usually) negative. The highest point above
                    // the baseline.
  SkScalar descent; // The lowest point below the baseline.
  SkScalar leading; // The suggested distance between lines (the bottom of the
                    // descent and the top of the ascent).

  // Temporaries used between `runBuffer` and `commitRunBuffer`.
  int glyph_count;
  uint32_t *clusters;
  int cluster_offset;
  // clusters[glyph] + cluster_offset is a position in the utf8_text.
};

SkShaper &GetShaper() {
  static std::unique_ptr<SkShaper> shaper =
      SkShaper::MakeShapeDontWrapOrReorder(SkUnicode::MakeIcuBasedUnicode());
  return *shaper;
}

void Font::DrawText(SkCanvas &canvas, std::string_view text, SkPaint &paint) {
  canvas.scale(font_scale, -font_scale);
  SkShaper &shaper = GetShaper();
  LineRunHandler run_handler(text.data(), SkPoint());
  shaper.shape(text.data(), text.size(), sk_font, true, 0, &run_handler);

  sk_sp<SkTextBlob> text_blob = run_handler.makeBlob();
  canvas.drawTextBlob(text_blob, 0, 0, paint);

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