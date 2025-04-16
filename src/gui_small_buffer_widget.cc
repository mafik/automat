// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#include "gui_small_buffer_widget.hh"

#include <include/core/SkPath.h>

#include "gui_constants.hh"
#include "gui_shape_widget.hh"
#include "log.hh"
#include "svg.hh"

namespace automat::gui {

static const SkPath kTypeUnsignedPath = PathFromSVG(kTypeUnsignedSVG, SVGUnit_Millimeters);
static const SkPath kTypeSignedPath = PathFromSVG(kTypeSignedSVG, SVGUnit_Millimeters);
static const SkPath kTypeHexPath = PathFromSVG(kTypeHexSVG, SVGUnit_Millimeters);
static const SkPath kTypeTextPath = PathFromSVG(kTypeTextSVG, SVGUnit_Millimeters);

struct TypeButton : Clickable {
  std::function<void()> on_click;
  SkRRect RRect() const override {
    return SkRRect::MakeOval(SkRect::MakeXYWH(-4_mm, -4_mm, 8_mm, 8_mm));
  }

  TypeButton(SkPath path) : Clickable(MakePtr<ShapeWidget>(path)) {}

  void Activate(gui::Pointer&) override {
    if (on_click) {
      on_click();
    }
  }
};

SmallBufferWidget::SmallBufferWidget(NestedWeakPtr<Buffer> buffer)
    : buffer_weak(buffer), type_button(MakePtr<TypeButton>(kTypeTextPath)) {
  auto tb = type_button.GetCast<TypeButton>();
  tb->on_click = [this]() {
    auto buffer = buffer_weak.Lock();
    auto old_type = buffer->GetBufferType();
    auto new_type = (Buffer::Type)(((int)old_type + 1) % (int)Buffer::Type::TypeCount);
    buffer->SetBufferType(new_type);
    WakeAnimation();
  };
}

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
      case Buffer::Type::Signed:
      case Buffer::Type::Unsigned: {
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
    case Buffer::Type::Signed:
      return gui::GetFont();
    case Buffer::Type::Unsigned:
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
  auto old_type = widget.type;
  auto new_type = buf->GetBufferType();
  bool type_changed = old_type != new_type;
  auto& text = widget.text;
  auto old_size = text.size();
  auto bytes = buf->BufferRead();
  if (new_type == Buffer::Type::Text) {
    text = bytes;
    while (text.ends_with('\0')) {
      text.pop_back();
    }
  } else {
    if (new_type == Buffer::Type::Signed) {
      if (bytes.size() == 1) {
        text = maf::f("%hhd", *(int8_t*)&bytes[0]);
      } else if (bytes.size() == 2) {
        text = maf::f("%hd", *(int16_t*)&bytes[0]);
      } else if (bytes.size() == 4) {
        text = maf::f("%d", *(int32_t*)&bytes[0]);
      } else if (bytes.size() == 8) {
        text = maf::f("%lld", *(int64_t*)&bytes[0]);
      } else {
        text = maf::f("%lld (size=%d)", *(int64_t*)&bytes[0], (int)bytes.size());
      }
    } else if (new_type == Buffer::Type::Unsigned) {
      if (bytes.size() == 1) {
        text = maf::f("%hhu", *(uint8_t*)&bytes[0]);
      } else if (bytes.size() == 2) {
        text = maf::f("%hu", *(uint16_t*)&bytes[0]);
      } else if (bytes.size() == 4) {
        text = maf::f("%u", *(uint32_t*)&bytes[0]);
      } else if (bytes.size() == 8) {
        text = maf::f("%llu", *(uint64_t*)&bytes[0]);
      } else {
        text = maf::f("%llu (size=%d)", *(uint64_t*)&bytes[0], (int)bytes.size());
      }
    } else if (new_type == Buffer::Type::Hexadecimal) {
      if (bytes.size() == 1) {
        text = maf::f("%hhx", (uint8_t)bytes[0]);
      } else if (bytes.size() == 2) {
        text = maf::f("%hx", *(uint16_t*)&bytes[0]);
      } else if (bytes.size() == 4) {
        text = maf::f("%x", *(uint32_t*)&bytes[0]);
      } else if (bytes.size() == 8) {
        text = maf::f("%llx", *(uint64_t*)&bytes[0]);
      } else {
        text = maf::f("%llx (size=%d)", *(uint64_t*)&bytes[0], (int)bytes.size());
      }
    } else {
      text = maf::f("(type=%d?)", (int)new_type);
    }
  }
  if (type_changed) {
    widget.type = new_type;
    auto shape_widget = widget.type_button->child.GetCast<ShapeWidget>();
    switch (new_type) {
      case Buffer::Type::Unsigned:
        shape_widget->path = kTypeUnsignedPath;
        break;
      case Buffer::Type::Signed:
        shape_widget->path = kTypeSignedPath;
        break;
      case Buffer::Type::Hexadecimal:
        shape_widget->path = kTypeHexPath;
        break;
      case Buffer::Type::Text:
        shape_widget->path = kTypeTextPath;
        break;
      default:
        break;
    }
    shape_widget->WakeAnimation();
    for (auto& [caret, pos] : widget.caret_positions) {
      if (pos.index > text.size() || pos.index == old_size) {
        pos.index = text.size();
      }
      widget.UpdateCaret(*caret);
    }
  }
}

animation::Phase SmallBufferWidget::Tick(time::Timer&) {
  RefreshText(*this);
  auto shape = Shape();
  auto bounds = shape.getBounds();

  type_button->local_to_parent =
      SkM44::Translate(bounds.right() - 4_mm, bounds.centerY()).preScale(0.666, 0.666);
  return animation::Finished;
}

void SmallBufferWidget::Draw(SkCanvas& canvas) const {
  auto default_matrix = canvas.getLocalToDevice();
  SkPaint background_paint;
  background_paint.setColor(SK_ColorWHITE);
  auto shape = Shape();
  canvas.drawPath(shape, background_paint);
  SkPaint text_paint;
  text_paint.setColor(SK_ColorBLACK);
  auto& font = GetFont(type);
  canvas.translate(kMargin, 0);
  font.DrawText(canvas, text, text_paint);
  canvas.setMatrix(default_matrix);
  DrawChildren(canvas);
}

void SmallBufferWidget::FillChildren(maf::Vec<Ptr<Widget>>& children) {
  children.push_back(type_button);
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
    // text has been modified - update the buffer
    auto buf = buffer_weak.Lock();
    buf->BufferVisit([&](std::span<char> span) {
      if (type == Buffer::Type::Text) {
        size_t n = std::min(span.size(), text.size());
        memcpy(span.data(), text.data(), n);
        if (n < span.size()) {
          memset(span.data() + n, 0, span.size() - n);
        }
      } else if (type == Buffer::Type::Signed) {
        int64_t value = 0;
        std::from_chars(text.data(), text.data() + text.size(), value);
        if (span.size() == 1 && value > 0x7f) {
          value = 0x7f;
        } else if (span.size() == 2 && value > 0x7fff) {
          value = 0x7fff;
        } else if (span.size() == 4 && value > 0x7fffffff) {
          value = 0x7fffffff;
        } else if (span.size() == 1 && value < -0x80) {
          value = -0x80;
        } else if (span.size() == 2 && value < -0x8000) {
          value = -0x8000;
        } else if (span.size() == 4 && value < -0x80000000) {
          value = -0x80000000;
        }
        memcpy(span.data(), &value, span.size());
      } else if (type == Buffer::Type::Unsigned) {
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
