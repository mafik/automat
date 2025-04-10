// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "gui_constants.hh"
#include "widget.hh"

namespace automat {

// Returns true if the buffer was modified.
using BufferVisitor = std::function<bool(std::span<char>)>;

struct Buffer {
  virtual void BufferVisit(const BufferVisitor&) = 0;

  // Default implementation relies on BufferVisit.
  virtual int BufferSize() {
    int size = 0;
    BufferVisit([&](std::span<char> span) {
      size += span.size();
      return false;  // not modified
    });
    return size;
  }

  virtual std::string BufferRead() {
    std::string result;
    BufferVisit([&](std::span<char> span) {
      result.append(span.data(), span.size());
      return false;  // not modified
    });
    return result;
  }

  virtual void BufferWrite(std::string_view new_value) {
    BufferVisit([&](std::span<char> span) {
      int n = std::min(span.size(), new_value.size());
      std::copy(new_value.begin(), new_value.begin() + n, span.begin());
      return true;  // modified
    });
  }

  enum class Type {
    Text,
    Integer,
    Hexadecimal,
  };

  virtual Type GetBufferType() { return Type::Text; };
  virtual bool IsBufferTypeMutable() { return false; }
  virtual void SetBufferType(Type new_type) {}
};

};  // namespace automat

namespace automat::gui {

// How to store the "mode" of the widget??
// It shouold be part of the Object (buffer "controller"?) - because it should be persistent.

// Can be used to edit a short string of bytes as a decimal number / hexadecimal number / short
// UTF-8 string.
struct SmallBufferWidget : Widget {
  static constexpr float kHeight = std::max(kLetterSize + 2 * kMargin, kMinimalTouchableSize);
  static constexpr float kCornerRadius = kHeight / 2;

  NestedWeakPtr<Buffer> buffer;

  float width;

  SmallBufferWidget(NestedWeakPtr<Buffer> buffer);

  RRect CoarseBounds() const override;
  SkPath Shape() const override;
  void Draw(SkCanvas&) const override;
};

}  // namespace automat::gui