// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "text_field.hh"

#include <include/core/SkColor.h>
#include <include/core/SkFontTypes.h>
#include <include/core/SkMatrix.h>
#include <src/base/SkUTF.h>

#include <memory>
#include <optional>

#include "animation.hh"
#include "base.hh"
#include "font.hh"
#include "gui_connection_widget.hh"
#include "root_widget.hh"

using namespace maf;

namespace automat::gui {

void TextFieldBase::PointerOver(Pointer& pointer) {
  pointer.PushIcon(Pointer::kIconIBeam);
  WakeAnimation();
}

void TextFieldBase::PointerLeave(Pointer& pointer) {
  pointer.PopIcon();
  WakeAnimation();
}

void DrawDebugTextOutlines(SkCanvas& canvas, std::string* text) {
  const char* c_str = text->c_str();
  size_t byte_length = text->size();
  Font& font = GetFont();
  int glyph_count = font.sk_font.countText(c_str, byte_length, SkTextEncoding::kUTF8);
  SkGlyphID glyphs[glyph_count];
  font.sk_font.textToGlyphs(c_str, byte_length, SkTextEncoding::kUTF8, glyphs, glyph_count);

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
    auto& b = bounds[i];
    canvas.drawRect(b, outline);
    canvas.drawLine(0, 0, widths[i], 0, line);
    canvas.drawCircle(0, 0, 0.5, line);
    canvas.translate(widths[i], 0);
  }
  canvas.scale(1 / font.font_scale, -1 / font.font_scale);
  canvas.restore();
}

SkRRect TextField::ShapeRRect() const {
  return SkRRect::MakeRectXY(SkRect::MakeXYWH(0, 0, width, kTextFieldHeight), kTextCornerRadius,
                             kTextCornerRadius);
}

static SkPaint kDefaultTextPaint = []() {
  SkPaint paint;
  paint.setColor(SK_ColorBLACK);
  paint.setAntiAlias(true);
  return paint;
}();

static SkPaint kDefaultBackgroundPaint = []() {
  SkPaint paint;
  paint.setColor(SK_ColorWHITE);
  paint.setAntiAlias(true);
  return paint;
}();

const SkPaint& TextField::GetTextPaint() const { return kDefaultTextPaint; }
const SkPaint& TextField::GetBackgroundPaint() const { return kDefaultBackgroundPaint; }

void TextField::Draw(SkCanvas& canvas) const {
  DrawBackground(canvas);
  DrawText(canvas);
}

void TextField::DrawBackground(SkCanvas& canvas) const {
  SkRRect rrect = ShapeRRect();
  canvas.drawRRect(rrect, GetBackgroundPaint());
}

void TextField::DrawText(SkCanvas& canvas) const {
  Font& font = GetFont();
  Vec2 text_pos = GetTextPos();
  SkRect underline_rect = SkRect::MakeXYWH(text_pos.x, text_pos.y - font.line_thickness * 2,
                                           width - 2 * kTextMargin, font.line_thickness);
  canvas.drawRect(underline_rect, GetTextPaint());
  canvas.translate(text_pos.x, text_pos.y);
  if (text) {
    font.DrawText(canvas, *text, GetTextPaint());
    // DrawDebugTextOutlines(canvas, text);
  }
}
void TextField::TextVisit(const TextVisitor& visitor) { visitor(*text); }
int TextField::IndexFromPosition(float local_x) const {
  Vec2 text_pos = GetTextPos();
  return GetFont().IndexFromPosition(*text, local_x - text_pos.x);
}
Vec2 TextField::PositionFromIndex(int index) const {
  return GetTextPos() + Vec2(GetFont().PositionFromIndex(*text, index), 0);
}
Vec2 TextField::GetTextPos() const {
  return Vec2(kTextMargin, (kTextFieldHeight - kLetterSize) / 2);
}

SkPath TextField::Shape() const { return SkPath::RRect(ShapeRRect()); }

void TextFieldBase::UpdateCaret(Caret& caret) {
  int index = caret_positions[&caret].index;
  Vec2 caret_pos = PositionFromIndex(index);
  caret.PlaceIBeam(caret_pos);
}

struct TextSelectAction : Action {
  TextFieldBase& text_field;
  Caret* caret = nullptr;

  // TextSelectionAction can be used to drag connections. In order to do this, make sure to set the
  // `argument` of the TextField.
  bool selecting_text = true;
  std::optional<DragConnectionAction> drag;

  TextSelectAction(Pointer& pointer, TextFieldBase& text_field)
      : Action(pointer), text_field(text_field) {
    if (text_field.argument.has_value()) {
      Location* location = Closest<Location>(*pointer.hover);

      for (auto& connection_widget : root_widget->connection_widgets) {
        if (&connection_widget->arg == text_field.argument &&
            &connection_widget->from == location) {
          drag.emplace(pointer, *connection_widget);
          break;
        }
      }
    }

    Vec2 local = pointer.PositionWithin(text_field);
    int index = text_field.IndexFromPosition(local.x);
    Vec2 pos = text_field.PositionFromIndex(index);
    caret = &pointer.keyboard->RequestCaret(text_field, pointer.hover, pos);
    text_field.caret_positions[caret] = {.index = index};
  }

  void UpdateCaretFromPointer(Pointer& pointer) {
    auto it = text_field.caret_positions.find(caret);
    // The caret might have been released.
    if (it == text_field.caret_positions.end()) {
      return;
    }
    Vec2 local = pointer.PositionWithin(text_field);
    if (drag.has_value()) {
      bool new_inside = text_field.Shape().contains(local.x, local.y);
      if (new_inside != selecting_text) {
        selecting_text = new_inside;
      }
    }

    if (selecting_text) {
      int index = text_field.IndexFromPosition(local.x);
      if (index != it->second.index) {
        it->second.index = index;
        text_field.UpdateCaret(*caret);
      }
    } else {
      drag->Update();
    }
  }

  void Update() override { UpdateCaretFromPointer(pointer); }
};

std::unique_ptr<Action> TextFieldBase::FindAction(Pointer& pointer, ActionTrigger btn) {
  if (btn == PointerButton::Left) {
    return std::make_unique<TextSelectAction>(pointer, *this);
  }
  return nullptr;
}

void TextFieldBase::ReleaseCaret(Caret& caret) { caret_positions.erase(&caret); }

std::string FilterControlCharacters(const std::string& text) {
  std::string clean = "";
  const char* ptr = text.c_str();
  const char* end = ptr + text.size();
  while (ptr < end) {
    const char* start = ptr;
    SkUnichar uni = SkUTF::NextUTF8(&ptr, end);
    if (uni < 0x20) {
      continue;
    }
    clean.append(start, ptr);
  }
  return clean;
}

void TextFieldBase::KeyDown(Caret& caret, Key k) {
  switch (k.physical) {
    case AnsiKey::Delete: {
      int begin = caret_positions[&caret].index;
      TextVisit([&](std::string& text) {
        int end = GetFont().NextIndex(text, begin);
        if (end != begin) {
          text.erase(begin, end - begin);
          return true;
        }
        return false;
      });
      break;
    }
    case AnsiKey::Backspace: {
      int end = caret_positions[&caret].index;
      if (end > 0) {
        I64 old_size;
        TextVisit([&](std::string& text) {
          old_size = text.size();
          int start = GetFont().PrevIndex(text, end);
          text.erase(start, end - start);
          return true;
        });
        I64 new_size = old_size;
        TextVisit([&](std::string& text) {
          new_size = text.size();
          return false;
        });
        caret_positions[&caret].index += new_size - old_size;
        UpdateCaret(caret);
      }
      break;
    }
    case AnsiKey::Left: {
      int& i_ref = caret_positions[&caret].index;
      if (i_ref > 0) {
        TextVisit([&](std::string& text) {
          i_ref = GetFont().PrevIndex(text, i_ref);
          return false;
        });
        UpdateCaret(caret);
      }
      break;
    }
    case AnsiKey::Right: {
      int& i_ref = caret_positions[&caret].index;
      TextVisit([&](std::string& text) {
        if (i_ref < text.size()) {
          i_ref = GetFont().NextIndex(text, i_ref);
          UpdateCaret(caret);
        }
        return false;
      });
      break;
    }
    case AnsiKey::Home: {
      caret_positions[&caret].index = 0;
      UpdateCaret(caret);
      break;
    }
    case AnsiKey::End: {
      TextVisit([&](std::string& text) {
        caret_positions[&caret].index = text.size();
        return false;
      });
      UpdateCaret(caret);
      break;
    }
    default: {
      std::string clean = FilterControlCharacters(k.text);
      if (!clean.empty()) {
        I64 old_size;
        TextVisit([&](std::string& text) {
          old_size = text.size();
          text.insert(caret_positions[&caret].index, clean);
          return true;  // because text is changed
        });
        I64 new_size = old_size;
        TextVisit([&](std::string& text) {
          new_size = text.size();
          return false;  // because text is not changed
        });
        caret_positions[&caret].index += new_size - old_size;
        UpdateCaret(caret);
      }
    }
  }
}

void TextFieldBase::KeyUp(Caret&, Key) {}

}  // namespace automat::gui
