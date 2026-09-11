#pragma once
// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT

#include <include/core/SkRRect.h>

#include "base.hpp"
#include "keyboard.hpp"
#include "pointer.hpp"
#include "toy.hpp"
#include "ui_constants.hpp"
#include "widget.hpp"

namespace automat::ui {

constexpr float kTextMargin = 0.001;
constexpr float kTextCornerRadius = kTextMargin;
constexpr float kTextFieldHeight = kMinimalTouchableSize;
constexpr float kTextFieldMinWidth = kTextFieldHeight;

struct CaretPosition {
  int index;  // byte offset within UTF-8 string
};

struct TextFieldBase : Toy {
  std::unordered_map<Caret*, CaretPosition> caret_positions;
  Str text;

  TextFieldBase(ui::Widget* parent, Object& owner, automat::Text::Table& table);

  Interface FindOption(Pointer&, ActionTrigger) override;
  Tock Tick(time::Timer&) override;

  // Update the given caret to its current position from `caret_positions`.
  void UpdateCaret(Caret& caret);
  void MoveCaret(Caret& caret, int index);

  void ReleaseCaret(Caret&) override;
  void KeyDown(Caret&, Key) override;
  void KeyUp(Caret&, Key) override;

  void SetText(StrView edited);
  virtual int IndexFromPosition(float x) const = 0;
  virtual Vec2 PositionFromIndex(int index) const = 0;
};

struct TextField : TextFieldBase {
  static constexpr float kHeight =
      std::max(kLetterSize + 2 * kMargin + 2 * kBorderWidth, kMinimalTouchableSize);

  float width;

  // TODO: just pass automat::Text (rather than separate owner + table)
  TextField(ui::Widget* parent, Object& owner, automat::Text::Table& table, float width)
      : TextFieldBase(parent, owner, table), width(width) {}
  void Draw(SkCanvas&) const override;
  SkPath Shape() const override;

  virtual Vec2 GetTextPos() const;
  int IndexFromPosition(float x) const override;
  Vec2 PositionFromIndex(int index) const override;

  // Internal method used to draw the background of the text field.
  //
  // Intended to be used internally by TextField::Draw. Subclasses may override this method to
  // customize background appearance.
  virtual void DrawBackground(SkCanvas&) const;
  virtual void DrawText(SkCanvas&) const;

  virtual SkRRect ShapeRRect() const;
  virtual const SkPaint& GetTextPaint() const;
  virtual const SkPaint& GetBackgroundPaint() const;
};

}  // namespace automat::ui
