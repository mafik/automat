#include "text_field.hh"

#include <include/core/SkColor.h>
#include <include/core/SkFontTypes.h>
#include <include/core/SkMatrix.h>
#include <src/base/SkUTF.h>

#include <memory>
#include <optional>

#include "base.hh"
#include "font.hh"
#include "format.hh"
#include "gui_connection_widget.hh"
#include "window.hh"

using namespace maf;

namespace automat::gui {

void TextField::PointerOver(Pointer& pointer, animation::Display& display) {
  pointer.PushIcon(Pointer::kIconIBeam);
  hover_ptr[display].Increment();
}

void TextField::PointerLeave(Pointer& pointer, animation::Display& display) {
  pointer.PopIcon();
  hover_ptr[display].Decrement();
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

animation::Phase TextField::Draw(DrawContext& ctx) const {
  auto& display = ctx.display;
  auto& hover = hover_ptr[display].animation;
  auto phase = hover.Tick(display);
  DrawBackground(ctx);
  DrawText(ctx);
  return phase;
}

void TextField::DrawBackground(DrawContext& ctx) const {
  auto& canvas = ctx.canvas;
  SkRRect rrect = ShapeRRect();
  canvas.drawRRect(rrect, GetBackgroundPaint());
  auto& display = ctx.display;
  auto& hover = hover_ptr[display].animation;
  if (hover.value > 0.0001) {
    SkPaint hover_outline;
    hover_outline.setColor(SkColorSetRGB(0xff, 0x00, 0x00));
    hover_outline.setStyle(SkPaint::kStroke_Style);
    hover_outline.setStrokeWidth(hover.value * 0.0005);
    canvas.drawRRect(rrect, hover_outline);
  }
}

void TextField::DrawText(DrawContext& ctx) const {
  Font& font = GetFont();
  Vec2 text_pos = GetTextPos();
  SkRect underline_rect = SkRect::MakeXYWH(text_pos.x, text_pos.y - font.line_thickness * 2,
                                           width - 2 * kTextMargin, font.line_thickness);
  ctx.canvas.drawRect(underline_rect, GetTextPaint());
  ctx.canvas.translate(text_pos.x, text_pos.y);
  if (text) {
    font.DrawText(ctx.canvas, *text, GetTextPaint());
    // DrawDebugTextOutlines(canvas, text);
  }
}

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

SkPath TextField::Shape(animation::Display*) const { return SkPath::RRect(ShapeRRect()); }

void UpdateCaret(TextField& text_field, Caret& caret) {
  int index = text_field.caret_positions[&caret].index;
  Vec2 caret_pos = text_field.PositionFromIndex(index);
  caret.PlaceIBeam(caret_pos);
}

struct TextSelectAction : Action {
  TextField& text_field;
  Caret* caret = nullptr;

  // TextSelectionAction can be used to drag connections. In order to do this, make sure to set the
  // `argument` of the TextField.
  bool selecting_text = true;
  std::optional<DragConnectionAction> drag;

  TextSelectAction(Pointer& pointer, TextField& text_field)
      : Action(pointer), text_field(text_field) {}

  void UpdateCaretFromPointer(Pointer& pointer) {
    auto it = text_field.caret_positions.find(caret);
    // The caret might have been released.
    if (it == text_field.caret_positions.end()) {
      return;
    }
    Vec2 local = pointer.PositionWithin(text_field);
    if (drag.has_value()) {
      bool new_inside = text_field.Shape(nullptr).contains(local.x, local.y);
      if (new_inside != selecting_text) {
        selecting_text = new_inside;
      }
    }

    if (selecting_text) {
      int index = text_field.IndexFromPosition(local.x);
      if (index != it->second.index) {
        it->second.index = index;
        UpdateCaret(text_field, *caret);
      }
    } else {
      drag->Update();
    }
  }

  void Begin() override {
    if (text_field.argument.has_value()) {
      auto pointer_path = pointer.path;
      Location* location = nullptr;
      for (int i = pointer_path.size() - 1; i >= 0; --i) {
        if ((location = dynamic_cast<Location*>(pointer_path[i]))) {
          break;
        }
      }

      for (auto& connection_widget : window->connection_widgets) {
        if (&connection_widget->arg == text_field.argument &&
            &connection_widget->from == location) {
          drag.emplace(pointer, *connection_widget);
          drag->Begin();
          break;
        }
      }
    }

    Vec2 local = pointer.PositionWithin(text_field);
    int index = text_field.IndexFromPosition(local.x);
    Vec2 pos = text_field.PositionFromIndex(index);
    caret = &pointer.keyboard->RequestCaret(text_field, pointer.path, pos);
    text_field.caret_positions[caret] = {.index = index};
  }
  void Update() override { UpdateCaretFromPointer(pointer); }
  void End() override {
    if (drag.has_value() && !selecting_text) {
      drag->End();
    }
  }
  gui::Widget* Widget() override {
    if (drag.has_value() && !selecting_text) {
      drag->Widget();
    }
    return nullptr;
  }
};

std::unique_ptr<Action> TextField::FindAction(Pointer& pointer, ActionTrigger btn) {
  if (btn == PointerButton::Left) {
    return std::make_unique<TextSelectAction>(pointer, *this);
  }
  return nullptr;
}

void TextField::ReleaseCaret(Caret& caret) { caret_positions.erase(&caret); }

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

void TextField::KeyDown(Caret& caret, Key k) {
  switch (k.physical) {
    case AnsiKey::Delete: {
      int begin = caret_positions[&caret].index;
      int end = GetFont().NextIndex(*text, begin);
      if (end != begin) {
        text->erase(begin, end - begin);
        // No need to update caret after delete.
      }
      break;
    }
    case AnsiKey::Backspace: {
      int& i_ref = caret_positions[&caret].index;
      int end = i_ref;
      if (i_ref > 0) {
        i_ref = GetFont().PrevIndex(*text, i_ref);
        text->erase(i_ref, end - i_ref);
        UpdateCaret(*this, caret);
      }
      break;
    }
    case AnsiKey::Left: {
      int& i_ref = caret_positions[&caret].index;
      if (i_ref > 0) {
        i_ref = GetFont().PrevIndex(*text, i_ref);
        UpdateCaret(*this, caret);
      }
      break;
    }
    case AnsiKey::Right: {
      int& i_ref = caret_positions[&caret].index;
      if (i_ref < text->size()) {
        i_ref = GetFont().NextIndex(*text, i_ref);
        UpdateCaret(*this, caret);
      }
      break;
    }
    case AnsiKey::Home: {
      caret_positions[&caret].index = 0;
      UpdateCaret(*this, caret);
      break;
    }
    case AnsiKey::End: {
      caret_positions[&caret].index = text->size();
      UpdateCaret(*this, caret);
      break;
    }
    default: {
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
  }
}

void TextField::KeyUp(Caret&, Key) {}

}  // namespace automat::gui
