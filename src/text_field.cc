#include "text_field.h"

#include <include/core/SkColor.h>
#include <include/core/SkFontTypes.h>
#include <include/core/SkMatrix.h>
#include <memory>

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

void TextField::OnFocus(bool focus, animation::State &animation_state) {
  has_focus = focus;
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
  for (int i = 0; i < glyph_count; ++i) {
    auto &b = bounds[i];
    canvas.drawRect(b, outline);
    canvas.translate(widths[i], 0);
  }
  canvas.scale(1 / font.font_scale, -1 / font.font_scale);
  canvas.restore();
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

struct TextSelectAction : Action {
  TextField *text_field;

  TextSelectAction(TextField *text_field) : text_field(text_field) {}

  void Begin(Pointer &pointer) override {
    vec2 local = pointer.PositionWithin(*text_field);

    const char *c_str = text_field->text->c_str();
    size_t byte_length = text_field->text->size();
    Font &font = GetFont();
    int glyph_count =
        font.sk_font.countText(c_str, byte_length, SkTextEncoding::kUTF8);
    SkGlyphID glyphs[glyph_count];
    font.sk_font.textToGlyphs(c_str, byte_length, SkTextEncoding::kUTF8, glyphs,
                              glyph_count);

    SkScalar widths[glyph_count];
    SkRect bounds[glyph_count];
    font.sk_font.getWidthsBounds(glyphs, glyph_count, widths, bounds, nullptr);

    int index = 0;

    // TODO: Find the index in the glyph array closest to the position.
    // TODO: Convert the index into caret position & set it in the caret.

    Caret &caret = text_field->RequestCaret(pointer.Keyboard());
    vec2 caret_pos = Vec2(kTextMargin, (kTextFieldHeight - kLetterSize) / 2);
    text_field->caret_positions[&caret] = {.index = index};

    SkMatrix root_to_text = root_machine->TransformToChild(text_field);
    SkMatrix text_to_root;

    if (!root_to_text.invert(&text_to_root)) {
      EVERY_N_SEC(60) { ERROR() << "Failed to invert matrix"; }
    }

    vec2 caret_pos_root = Vec2(text_to_root.mapXY(caret_pos.X, caret_pos.Y));

    caret.PlaceIBeam(caret_pos_root);
  }
  void Update(Pointer &pointer) override {}
  void End() override {}
  void Draw(SkCanvas &canvas, animation::State &animation_state) override {}
};

std::unique_ptr<Action> TextField::ButtonDownAction(Pointer &, Button btn,
                                                    vec2 contact_point) {
  if (btn == Button::kMouseLeft) {
    return std::make_unique<TextSelectAction>(this);
  }
  return nullptr;
}

void TextField::KeyDown(Caret &caret, Key k) {
  // TODO: skip non-printable characters
  *text += k.text;
  caret_positions[&caret].index += k.text.size();
  std::string text_hex = "";
  for (char c : k.text) {
    text_hex += f(" %02x", (uint8_t)c);
  }
  LOG() << "Key down: " << k.text << " (" << text_hex << ")";
}

void TextField::KeyUp(Caret &, Key) {}

} // namespace automaton::gui
