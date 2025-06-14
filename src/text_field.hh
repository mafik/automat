// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <include/core/SkRRect.h>

#include "gui_constants.hh"
#include "keyboard.hh"
#include "pointer.hh"
#include "widget.hh"

namespace automat {
struct Argument;
}  // namespace automat

namespace automat::gui {

constexpr float kTextMargin = 0.001;
constexpr float kTextCornerRadius = kTextMargin;
constexpr float kTextFieldHeight = kMinimalTouchableSize;
constexpr float kTextFieldMinWidth = kTextFieldHeight;

struct CaretPosition {
  int index;  // byte offset within UTF-8 string
};

// Returns true if the value has been modified
using TextVisitor = std::function<bool(std::string&)>;

struct TextFieldBase : Widget, CaretOwner {
  std::unordered_map<Caret*, CaretPosition> caret_positions;
  std::optional<Argument*> argument;
  Optional<Pointer::IconOverride> ibeam_icon;

  void PointerOver(Pointer&) override;
  void PointerLeave(Pointer&) override;

  std::unique_ptr<Action> FindAction(Pointer&, ActionTrigger) override;

  // Update the given caret to its current position from `caret_positions`.
  void UpdateCaret(Caret& caret);

  void ReleaseCaret(Caret&) override;
  void KeyDown(Caret&, Key) override;
  void KeyUp(Caret&, Key) override;

  virtual void TextVisit(const TextVisitor&) = 0;
  virtual int IndexFromPosition(float x) const = 0;
  virtual Vec2 PositionFromIndex(int index) const = 0;
};

struct TextField : TextFieldBase {
  static constexpr float kHeight =
      std::max(kLetterSize + 2 * kMargin + 2 * kBorderWidth, kMinimalTouchableSize);

  std::string* text;
  float width;

  TextField(std::string* text, float width) : text(text), width(width) {}
  void Draw(SkCanvas&) const override;
  SkPath Shape() const override;

  virtual Vec2 GetTextPos() const;
  void TextVisit(const TextVisitor&) override;
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

}  // namespace automat::gui