// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "text_field.hpp"

#include <include/core/SkColor.h>
#include <include/core/SkFontTypes.h>
#include <include/core/SkMatrix.h>
#include <src/base/SkUTF.h>

#include <memory>
#include <optional>

#include "animation.hpp"
#include "base.hpp"
#include "font.hpp"
#include "object.hpp"
#include "root_widget.hpp"

namespace automat::ui {

void DrawDebugTextOutlines(SkCanvas& canvas, std::string* text) {
  const char* c_str = text->c_str();
  size_t byte_length = text->size();
  Font& font = GetFont();
  int glyph_count = font.sk_font.countText(c_str, byte_length, SkTextEncoding::kUTF8);
  SkGlyphID glyphs[glyph_count];
  auto glyph_span = SkSpan<SkGlyphID>(glyphs, glyph_count);
  font.sk_font.textToGlyphs(c_str, byte_length, SkTextEncoding::kUTF8, glyph_span);

  SkScalar widths[glyph_count];
  SkRect bounds[glyph_count];
  font.sk_font.getWidthsBounds(glyph_span, SkSpan<SkScalar>(widths, glyph_count),
                               SkSpan<SkRect>(bounds, glyph_count), nullptr);

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

static SkPaint kDefaultTextPaint = [] {
  SkPaint paint;
  paint.setColor(SK_ColorBLACK);
  paint.setAntiAlias(true);
  return paint;
}();

static SkPaint kDefaultBackgroundPaint = [] {
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
  font.DrawText(canvas, text, GetTextPaint());
}
int TextField::IndexFromPosition(float local_x) const {
  Vec2 text_pos = GetTextPos();
  return GetFont().IndexFromPosition(text, local_x - text_pos.x);
}
Vec2 TextField::PositionFromIndex(int index) const {
  return GetTextPos() + Vec2(GetFont().PositionFromIndex(text, index), 0);
}
Vec2 TextField::GetTextPos() const {
  return Vec2(kTextMargin, (kTextFieldHeight - kLetterSize) / 2);
}

SkPath TextField::Shape() const { return SkPath::RRect(ShapeRRect()); }

TextFieldBase::TextFieldBase(ui::Widget* parent, Object& owner, automat::Text::Table& table)
    : Toy(parent, owner, &table, owner.wake_counter) {}

Interface TextFieldBase::FindOption(Pointer&, ActionTrigger trigger) {
  if (trigger != PointerButton::Left) return {};
  auto object = LockOwner();
  if (!object) return {};
  return Interface(*object, *iface);
}

Widget::Tock TextFieldBase::Tick(time::Timer&) {
  auto bound = LockBind<automat::Text>();
  Str fresh = bound ? bound.Get() : Str();
  if (fresh == text) return {};
  size_t old_size = text.size();
  text = std::move(fresh);
  for (auto& [caret, pos] : caret_positions) {
    if (pos.index > text.size() || pos.index == old_size) pos.index = text.size();
    UpdateCaret(*caret);
  }
  return Tock::Draw;
}

void TextFieldBase::SetText(StrView edited) {
  if (auto bound = LockBind<automat::Text>()) bound.Set(edited);
  WakeAnimation();
}

void TextFieldBase::UpdateCaret(Caret& caret) {
  int index = caret_positions[&caret].index;
  Vec2 caret_pos = PositionFromIndex(index);
  caret.PlaceIBeam(caret_pos);
}

void TextFieldBase::MoveCaret(Caret& caret, int index) {
  caret_positions[&caret].index = std::min<int>(index, text.size());
  UpdateCaret(caret);
}

struct TextSelectAction : Action {
  MortalPtr<TextFieldBase> field;
  MortalPtr<ui::Caret> caret;

  TextSelectAction(Pointer& pointer, TextFieldBase& text_field)
      : Action(pointer), field(&text_field) {
    if (pointer.keyboard) {
      Vec2 local = pointer.PositionWithin(text_field);
      int index = text_field.IndexFromPosition(local.x);
      Vec2 pos = text_field.PositionFromIndex(index);
      caret = &pointer.keyboard->RequestCaret(text_field, pos);
      text_field.caret_positions[caret] = {.index = index};
    }
  }

  void Update() override {
    if (!field) return;
    auto it = field->caret_positions.find(caret);
    // The caret might have been released.
    if (it == field->caret_positions.end()) return;
    Vec2 local = pointer.PositionWithin(*field);
    int index = field->IndexFromPosition(local.x);
    if (index != it->second.index) {
      it->second.index = index;
      field->UpdateCaret(*caret);
    }
  }
};

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
  int index = caret_positions[&caret].index;
  Font& font = GetFont();
  switch (k.physical) {
    case AnsiKey::Delete: {
      int end = font.NextIndex(text, index);
      if (end != index) {
        Str edited = text;
        edited.erase(index, end - index);
        SetText(edited);
      }
      break;
    }
    case AnsiKey::Backspace: {
      if (index > 0) {
        int start = font.PrevIndex(text, index);
        Str edited = text;
        edited.erase(start, index - start);
        SetText(edited);
        MoveCaret(caret, start);
      }
      break;
    }
    case AnsiKey::Left: {
      if (index > 0) MoveCaret(caret, font.PrevIndex(text, index));
      break;
    }
    case AnsiKey::Right: {
      if (index < text.size()) MoveCaret(caret, font.NextIndex(text, index));
      break;
    }
    case AnsiKey::Home: {
      MoveCaret(caret, 0);
      break;
    }
    case AnsiKey::End: {
      MoveCaret(caret, text.size());
      break;
    }
    default: {
      std::string clean = FilterControlCharacters(k.text);
      if (!clean.empty()) {
        Str edited = text;
        edited.insert(index, clean);
        SetText(edited);
        MoveCaret(caret, index + clean.size());
      }
    }
  }
}

void TextFieldBase::KeyUp(Caret&, Key) {}

}  // namespace automat::ui

namespace automat {

std::unique_ptr<Action> Text::Table::DefaultActivate(Interface self, ui::Pointer& pointer,
                                                     Toy* toy) {
  auto* field = toy ? dynamic_cast<ui::TextFieldBase*>(toy->FindWidget(self.table_ptr)) : nullptr;
  if (!field) return nullptr;
  return std::make_unique<ui::TextSelectAction>(pointer, *field);
}

}  // namespace automat
