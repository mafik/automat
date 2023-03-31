#pragma once

#include "gui.h"

namespace automaton::gui {

constexpr float kTextMargin = 0.001;
constexpr float kTextFieldHeight = 0.008; // 8mm
constexpr float kTextFieldMinWidth = kTextFieldHeight;

struct TextField : Widget {
  std::string *text;
  float width;
  dual_ptr<bool> has_hover;
  bool has_focus;

  TextField(std::string *text, float width) : text(text), width(width) {}
  void OnHover(bool hover, dual_ptr_holder& animation_state) override;
  void OnFocus(bool focus, dual_ptr_holder& animation_state) override;
  void Draw(SkCanvas &, dual_ptr_holder& animation_state) override;
  SkPath GetShape() override;
  std::unique_ptr<Action> KeyDown(Key) override;
  bool CanFocusKeyboard() override { return true; }
};

} // namespace automaton::gui