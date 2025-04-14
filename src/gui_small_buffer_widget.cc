// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#include "gui_small_buffer_widget.hh"

#include <include/core/SkPath.h>

#include "gui_constants.hh"
#include "log.hh"

namespace automat::gui {

SmallBufferWidget::SmallBufferWidget(NestedWeakPtr<Buffer> buffer) : buffer_weak(buffer) {}

void SmallBufferWidget::Measure() {
  width = kMinimalTouchableSize;
  auto buf = buffer_weak.Lock();
  int bytes = buf->BufferSize();

  float max_text_height = 0;
  auto AdjustSizeForBufferType = [&](Buffer::Type type) {
    auto& font = GetFont(type);
    max_text_height = std::max(max_text_height, font.letter_height);
    std::string sample_text = "";
    switch (type) {
      case Buffer::Type::Text:
        sample_text = std::string(bytes, 'W');
        break;
      case Buffer::Type::Integer: {
        uint64_t value = 0;
        for (int i = 0; i < bytes; i++) {
          value = (value << 8) | 0xff;
        }
        sample_text = std::to_string(value);
      } break;
      case Buffer::Type::Hexadecimal: {
        for (int i = 0; i < bytes; i++) {
          sample_text += maf::f("%02x", (uint8_t)0xff);
        }
        break;
      }
      default:
        break;
    }
    width = std::max(width, font.MeasureText(sample_text));
  };

  if (buf->IsBufferTypeMutable()) {
    for (int i = 0; i < (int)Buffer::Type::TypeCount; i++) {
      AdjustSizeForBufferType((Buffer::Type)i);
    }
    float button_size = max_text_height;
    width += kMargin + button_size;
  } else {
    AdjustSizeForBufferType(buf->GetBufferType());
  }
  height = max_text_height + kMargin * 2;
  vertical_margin = kMargin;
  if (height < kMinimalTouchableSize) {
    vertical_margin = (kMinimalTouchableSize - max_text_height) / 2;
    height = kMinimalTouchableSize;
  }
  width += kMargin * 2;
}

Font& SmallBufferWidget::GetFont(Buffer::Type type) const {
  int i = (int)type;
  if (fonts[i] != nullptr) {
    return *fonts[i];
  }
  switch (type) {
    case Buffer::Type::Text:
      return gui::GetFont();
    case Buffer::Type::Integer:
      return gui::GetFont();
    case Buffer::Type::Hexadecimal:
      return gui::GetFont();
    default:
      return gui::GetFont();
  }
}

static void RefreshText(SmallBufferWidget& widget) {
  auto buf = widget.buffer_weak.Lock();
  if (!buf) {
    return;
  }
  widget.type = buf->GetBufferType();
  auto& text = widget.text;
  text = buf->BufferRead();
  if (widget.type != Buffer::Type::Text) {
    int64_t value = 0;
    memcpy(&value, text.data(), text.size());
    if (widget.type == Buffer::Type::Integer) {
      text = maf::f("%d", value);
    } else if (widget.type == Buffer::Type::Hexadecimal) {
      text = maf::f("%x", value);
    }
  }
}

animation::Phase SmallBufferWidget::Tick(time::Timer&) {
  RefreshText(*this);
  return animation::Finished;
}

void SmallBufferWidget::Draw(SkCanvas& canvas) const {
  SkPaint background_paint;
  background_paint.setColor(SK_ColorWHITE);
  canvas.drawPath(Shape(), background_paint);
  SkPaint text_paint;
  text_paint.setColor(SK_ColorBLACK);
  auto& font = GetFont(type);
  canvas.translate(kMargin, 0);
  font.DrawText(canvas, text, text_paint);
}

RRect SmallBufferWidget::CoarseBounds() const {
  return RRect::MakeSimple(
      Rect::MakeAtZero<::LeftX, ::BottomY>(width, height).MoveBy({0, -vertical_margin}),
      height / 2);
}

SkPath SmallBufferWidget::Shape() const { return SkPath::RRect(CoarseBounds().sk); }

static constexpr Vec2 kTextPos = {kMargin, 0};

int SmallBufferWidget::IndexFromPosition(float local_x) const {
  return GetFont(type).IndexFromPosition(text, local_x - kTextPos.x);
}
Vec2 SmallBufferWidget::PositionFromIndex(int index) const {
  return kTextPos + Vec2(GetFont(type).PositionFromIndex(text, index), 0);
}

void SmallBufferWidget::TextVisit(const TextVisitor& visitor) {
  if (visitor(text)) {
    LOG << "Setting text to [" << text << "]";
    // text has been modified - update the buffer
    auto buf = buffer_weak.Lock();
    buf->BufferVisit([&](std::span<char> span) {
      if (type == Buffer::Type::Text) {
        size_t n = std::min(span.size(), text.size());
        memcpy(span.data(), text.data(), n);
        if (n < span.size()) {
          memset(span.data() + n, 0, span.size() - n);
        }
      } else if (type == Buffer::Type::Integer) {
        int64_t value = 0;
        std::from_chars(text.data(), text.data() + text.size(), value);
        if (span.size() == 1 && value > 0xff) {
          value = 0xff;
        } else if (span.size() == 2 && value > 0xffff) {
          value = 0xffff;
        } else if (span.size() == 4 && value > 0xffffffff) {
          value = 0xffffffff;
        }
        memcpy(span.data(), &value, span.size());
      } else if (type == Buffer::Type::Hexadecimal) {
        uint64_t value = 0;
        std::from_chars(text.data(), text.data() + text.size(), value, 16);
        memcpy(span.data(), &value, span.size());
      } else {
        ERROR << "Unsupported buffer type " << (int)type;
      }
      return true;
    });
    RefreshText(*this);
    WakeAnimation();
  }
}

}  // namespace automat::gui
