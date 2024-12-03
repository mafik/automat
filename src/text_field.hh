// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <include/core/SkRRect.h>

#include "animation.hh"
#include "gui_constants.hh"
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

struct TextField : Widget, CaretOwner {
  static constexpr float kHeight =
      std::max(kLetterSize + 2 * kMargin + 2 * kBorderWidth, kMinimalTouchableSize);

  std::string* text;
  float width;

  std::unordered_map<Caret*, CaretPosition> caret_positions;
  struct HoverState {
    int hovering_pointers = 0;
    float animation = 0;
    void Increment() { hovering_pointers++; }
    void Decrement() { hovering_pointers--; }
  };
  mutable HoverState hover;
  std::optional<Argument*> argument;

  TextField(std::string* text, float width) : text(text), width(width) {}
  void PointerOver(Pointer&) override;
  void PointerLeave(Pointer&) override;
  animation::Phase Update(time::Timer&) override;
  animation::Phase Draw(DrawContext&) const override;
  SkPath Shape() const override;
  std::unique_ptr<Action> FindAction(Pointer&, ActionTrigger) override;
  void ReleaseCaret(Caret&) override;
  Widget* CaretWidget() override { return this; }
  void KeyDown(Caret&, Key) override;
  void KeyUp(Caret&, Key) override;

  int IndexFromPosition(float x) const;
  Vec2 PositionFromIndex(int index) const;

  virtual Vec2 GetTextPos() const;

  // Internal method used to draw the background of the text field.
  //
  // Intended to be used internally by TextField::Draw. Subclasses may override this method to
  // customize background appearance.
  virtual void DrawBackground(DrawContext&) const;
  virtual void DrawText(DrawContext&) const;

  virtual SkRRect ShapeRRect() const;
  virtual const SkPaint& GetTextPaint() const;
  virtual const SkPaint& GetBackgroundPaint() const;
};

}  // namespace automat::gui