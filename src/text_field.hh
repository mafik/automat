#pragma once

#include "animation.hh"
#include "gui_constants.hh"
#include "widget.hh"

namespace automat::gui {

constexpr float kTextMargin = 0.001;
constexpr float kTextCornerRadius = kTextMargin;
constexpr float kTextFieldHeight = kMinimalTouchableSize;
constexpr float kTextFieldMinWidth = kTextFieldHeight;

struct CaretPosition {
  int index;  // byte offset within UTF-8 string
};

struct TextField : Widget, CaretOwner {
  std::string* text;
  float width;

  std::unordered_map<Caret*, CaretPosition> caret_positions;
  struct HoverState {
    int hovering_pointers = 0;
    animation::Approach animation;
    void Increment() {
      hovering_pointers++;
      animation.target = 1;
    }
    void Decrement() {
      hovering_pointers--;
      if (hovering_pointers == 0) {
        animation.target = 0;
      }
    }
  };
  mutable product_ptr<HoverState> hover_ptr;

  TextField(std::string* text, float width) : text(text), width(width) {}
  void PointerOver(Pointer&, animation::Context&) override;
  void PointerLeave(Pointer&, animation::Context&) override;
  void Draw(DrawContext&) const override;
  SkPath Shape() const override;
  std::unique_ptr<Action> ButtonDownAction(Pointer&, PointerButton) override;
  void ReleaseCaret(Caret&) override;
  Widget* CaretWidget() override { return this; }
  void KeyDown(Caret&, Key) override;
  void KeyUp(Caret&, Key) override;
};

}  // namespace automat::gui