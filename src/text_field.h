#pragma once

#include "animation.h"
#include "widget.h"

namespace automaton::gui {

constexpr float kTextMargin = 0.001;
constexpr float kTextFieldHeight = 0.008; // 8mm
constexpr float kTextFieldMinWidth = kTextFieldHeight;

struct TextField : Widget {
  Widget *parent_widget;
  std::string *text;
  float width;
  bool has_focus;
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
  std::vector<std::unique_ptr<Caret>> carets;

  TextField(Widget *parent_widget, std::string *text, float width)
      : parent_widget(parent_widget), text(text), width(width) {}
  Widget *ParentWidget() override;
  void PointerOver(Pointer &, animation::State &) override;
  void PointerLeave(Pointer &, animation::State &) override;
  void OnFocus(bool focus, animation::State &animation_state) override;
  void Draw(SkCanvas &, animation::State &animation_state) const override;
  SkPath Shape() const override;
  std::unique_ptr<Action> KeyDownAction(Key) override;
  std::unique_ptr<Action> ButtonDownAction(Pointer &, Button,
                                           vec2 contact_point) override;
  bool CanFocusKeyboard() override { return true; }
};

} // namespace automaton::gui