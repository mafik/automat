#include "text_field.h"

#include <memory>
#include <numeric>

#include <include/core/SkColor.h>
#include <include/core/SkFontTypes.h>
#include <include/core/SkMatrix.h>
#include <src/base/SkUTF.h>

#include "font.h"
#include "log.h"
#include "root.h"

namespace automaton::gui {

Widget *TextField::ParentWidget() { return parent_widget; }

void TextField::PointerOver(Pointer &pointer, animation::State &state) {
  pointer.PushIcon(Pointer::kIconIBeam);
  hover_ptr[state].Increment();
}

void TextField::PointerLeave(Pointer &pointer, animation::State &state) {
  pointer.PopIcon();
  hover_ptr[state].Decrement();
}

void DrawDebugTextOutlines(SkCanvas &canvas, std::string *text) {
  const char *c_str = text->c_str();
  size_t byte_length = text->size();
  Font &font = GetFont();
  int glyph_count =
      font.sk_font.countText(c_str, byte_length, SkTextEncoding::kUTF8);
  SkGlyphID glyphs[glyph_count];
  font.sk_font.textToGlyphs(c_str, byte_length, SkTextEncoding::kUTF8, glyphs,
                            glyph_count);

  SkScalar widths[glyph_count];
  SkRect bounds[glyph_count];
  font.sk_font.getWidthsBounds(glyphs, glyph_count, widths, bounds, nullptr);

  // Draw glyph outlines for debugging
  canvas.save();
  canvas.scale(font.font_scale, -font.font_scale);
  SkPaint outline;
  outline.setStyle(SkPaint::kStroke_Style);
  outline.setColor(SkColorSetRGB(0xff, 0x00, 0x00));
  SkPaint line;
  line.setStyle(SkPaint::kStroke_Style);
  line.setColor(SkColorSetRGB(0x00, 0x80, 0x00));
  for (int i = 0; i < glyph_count; ++i) {
    auto &b = bounds[i];
    canvas.drawRect(b, outline);
    canvas.drawLine(0, 0, widths[i], 0, line);
    canvas.drawCircle(0, 0, 0.5, line);
    canvas.translate(widths[i], 0);
  }
  canvas.scale(1 / font.font_scale, -1 / font.font_scale);
  canvas.restore();
}

int GetCaretIndexFromPosition(const std::string &text, float x) {
  Font &font = GetFont();
  x /= font.font_scale;
  size_t byte_length = text.size();
  SkGlyphID glyphs[byte_length]; // We can't gave more glyphs than bytes.
  int glyph_count = font.sk_font.textToGlyphs(
      text.data(), byte_length, SkTextEncoding::kUTF8, glyphs, byte_length);
  SkScalar xpos[glyph_count];
  SkScalar widths[glyph_count];
  font.sk_font.getXPos(glyphs, glyph_count, xpos);
  font.sk_font.getWidths(glyphs, glyph_count, widths);

  const char *ptr = text.c_str();
  const char *end = ptr + byte_length;
  for (int i_glyph = 0; i_glyph < glyph_count; ++i_glyph) {
    if (x < xpos[i_glyph] + widths[i_glyph] / 2) {
      return ptr - text.c_str();
    }
    SkUTF::NextUTF8(&ptr, end);
  }
  return byte_length;
}

float CaretPositionFromIndex(const std::string &text, int index) {
  if (index == 0) {
    return 0;
  }
  Font &font = GetFont();
  size_t byte_length =
      index; // We're only interested in the first `index` bytes.
  SkGlyphID glyphs[byte_length]; // We can't gave more glyphs than bytes.
  int glyph_count = font.sk_font.textToGlyphs(
      text.data(), byte_length, SkTextEncoding::kUTF8, glyphs, byte_length);
  SkScalar xpos[glyph_count];
  SkScalar widths[glyph_count];
  font.sk_font.getXPos(glyphs, glyph_count, xpos);
  font.sk_font.getWidths(glyphs, glyph_count, widths);
  return (xpos[glyph_count - 1] + widths[glyph_count - 1]) * font.font_scale;
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
  canvas.translate(kTextMargin, (kTextFieldHeight - kLetterSize) / 2);
  SkPaint underline;
  underline.setColor(c_fg);
  SkRect underline_rect = SkRect::MakeXYWH(
      0, -font.line_thickness, width - 2 * kTextMargin, -font.line_thickness);
  canvas.drawRect(underline_rect, underline);
  SkPaint text_fg;
  text_fg.setColor(c_fg);
  if (text) {
    font.DrawText(canvas, *text, text_fg);
    DrawDebugTextOutlines(canvas, text);
  }
}

SkPath TextField::Shape() const {
  SkRect bounds = SkRect::MakeXYWH(0, 0, width, kTextFieldHeight);
  return SkPath::RRect(bounds, kTextMargin, kTextMargin);
}

std::unique_ptr<Action> TextField::KeyDownAction(Key) { return nullptr; }

void UpdateCaret(TextField &text_field, Caret &caret) {
  int index = text_field.caret_positions[&caret].index;
  vec2 caret_pos =
      Vec2(kTextMargin + CaretPositionFromIndex(*text_field.text, index),
           (kTextFieldHeight - kLetterSize) / 2);

  SkMatrix root_to_text = root_machine->TransformToChild(&text_field);
  SkMatrix text_to_root;

  if (!root_to_text.invert(&text_to_root)) {
    EVERY_N_SEC(60) { ERROR() << "Failed to invert matrix"; }
  }

  vec2 caret_pos_root = Vec2(text_to_root.mapXY(caret_pos.X, caret_pos.Y));

  caret.PlaceIBeam(caret_pos_root);
}

struct TextSelectAction : Action {
  TextField &text_field;

  TextSelectAction(TextField &text_field) : text_field(text_field) {}

  void Begin(Pointer &pointer) override {
    vec2 local = pointer.PositionWithin(text_field);
    int index =
        GetCaretIndexFromPosition(*text_field.text, local.X - kTextMargin);

    Caret &caret = text_field.RequestCaret(pointer.Keyboard());
    text_field.caret_positions[&caret] = {.index = index};

    UpdateCaret(text_field, caret);
  }
  void Update(Pointer &pointer) override {}
  void End() override {}
  void Draw(SkCanvas &canvas, animation::State &animation_state) override {}
};

std::unique_ptr<Action> TextField::ButtonDownAction(Pointer &, Button btn,
                                                    vec2 contact_point) {
  if (btn == Button::kMouseLeft) {
    return std::make_unique<TextSelectAction>(*this);
  }
  return nullptr;
}

void TextField::ReleaseCaret(Caret &caret) { caret_positions.erase(&caret); }

std::string FilterControlCharacters(const std::string &text) {
  std::string clean = "";
  const char *ptr = text.c_str();
  const char *end = ptr + text.size();
  while (ptr < end) {
    const char *start = ptr;
    SkUnichar uni = SkUTF::NextUTF8(&ptr, end);
    if (uni < 0x20) {
      continue;
    }
    clean.append(start, ptr);
  }
  return clean;
}

bool IsUtf8Continuation(uint8_t byte) {
  return (byte & 0b1100'0000) == 0b1000'0000;
}

void TextField::KeyDown(Caret &caret, Key k) {
  // TODO: delete can be used to delete next character
  if (k.physical == AnsiKey::Backspace) {
    int& i_ref = caret_positions[&caret].index;
    int end = i_ref;
    if (i_ref > 0) {
      i_ref--;
      uint8_t c = (*text)[i_ref];
      while (IsUtf8Continuation(c)) {
        i_ref--;
        c = (*text)[i_ref];
      }
      text->erase(i_ref, end - i_ref);
      UpdateCaret(*this, caret);
    }
    return;
  }
  if (k.physical == AnsiKey::Left) {
    int& i_ref = caret_positions[&caret].index;
    if (i_ref > 0) {
      i_ref--;
      uint8_t c = (*text)[i_ref];
      while (IsUtf8Continuation(c)) {
        i_ref--;
        c = (*text)[i_ref];
      }
      UpdateCaret(*this, caret);
    }
    return;
  }
  if (k.physical == AnsiKey::Right) {
    int& i_ref = caret_positions[&caret].index;
    if (i_ref < text->size()) {
      i_ref++;
      uint8_t c = (*text)[i_ref];
      while (IsUtf8Continuation(c)) {
        i_ref++;
        c = (*text)[i_ref];
      }
      UpdateCaret(*this, caret);
    }
    return;
  }
  if (k.physical == AnsiKey::Home) {
    caret_positions[&caret].index = 0;
    UpdateCaret(*this, caret);
    return;
  }
  if (k.physical == AnsiKey::End) {
    caret_positions[&caret].index = text->size();
    UpdateCaret(*this, caret);
    return;
  }
  std::string clean = FilterControlCharacters(k.text);
  if (!clean.empty()) {
    text->insert(caret_positions[&caret].index, clean);
    caret_positions[&caret].index += clean.size();
    UpdateCaret(*this, caret);
    std::string text_hex = "";
    for (char c : clean) {
      text_hex += f(" %02x", (uint8_t)c);
    }
  }
}

void TextField::KeyUp(Caret &, Key) {}

} // namespace automaton::gui
