// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "font.hh"
#include "gui_button.hh"
#include "text_field.hh"

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
    Unsigned,
    Signed,
    Hexadecimal,
    TypeCount,
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
struct SmallBufferWidget : TextFieldBase {
  NestedWeakPtr<Buffer> buffer_weak;
  Ptr<Clickable> type_button;

  gui::Font* fonts[(int)Buffer::Type::TypeCount] = {};

  float vertical_margin;
  float width;
  float height;
  Buffer::Type type = Buffer::Type::Hexadecimal;
  std::string text;

  SmallBufferWidget(NestedWeakPtr<Buffer> buffer);

  // Call this after setting the fonts to calculate the size of the widget.
  void Measure();

  Font& GetFont(Buffer::Type) const;

  RRect CoarseBounds() const override;
  SkPath Shape() const override;
  animation::Phase Tick(time::Timer&) override;
  void Draw(SkCanvas&) const override;

  void FillChildren(maf::Vec<Ptr<Widget>>& children) override;

  void TextVisit(const TextVisitor&) override;
  int IndexFromPosition(float x) const override;
  Vec2 PositionFromIndex(int index) const override;
};

}  // namespace automat::gui