// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#include "gui_small_buffer_widget.hh"

#include <include/core/SkPath.h>

#include "gui_constants.hh"
#include "gui_shape_widget.hh"
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
    auto new_type = (Buffer::Type)(((int)type + 1) % (int)Buffer::Type::TypeCount);
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
  if (old_type != new_type) {
    widget.type = buf->GetBufferType();
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
  }
  auto& text = widget.text;
  text = buf->BufferRead();
  if (widget.type != Buffer::Type::Text) {
    int64_t value = 0;
    memcpy(&value, text.data(), text.size());

    if (widget.type == Buffer::Type::Signed) {
      text = maf::f("%lld", value);
    } else if (widget.type == Buffer::Type::Unsigned) {
      text = maf::f("%llu", value);
    } else if (widget.type == Buffer::Type::Hexadecimal) {
      text = maf::f("%llx", value);
    }
  }
}

animation::Phase SmallBufferWidget::Tick(time::Timer&) {
  RefreshText(*this);
  auto shape = Shape();
  auto bounds = shape.getBounds();

  type_button->local_to_parent =
      SkM44::Translate(bounds.right() - 4_mm, bounds.centerY()).preScale(0.5, 0.5);
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
  auto btn_shape = type_button->Shape();
  btn_shape.transform(type_button->local_to_parent.asM33());
  SkPaint outline_paint;
  outline_paint.setColor(SK_ColorRED);
  outline_paint.setStyle(SkPaint::kStroke_Style);
  canvas.drawPath(btn_shape, outline_paint);
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
