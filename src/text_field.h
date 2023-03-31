#pragma once

#include "animation.h"
#include "gui.h"

namespace automaton::gui {

constexpr float kTextMargin = 0.001;
constexpr float kTextFieldHeight = 0.008; // 8mm
constexpr float kTextFieldMinWidth = kTextFieldHeight;

struct TextField : Widget {
  std::string *text;
  float width;
  bool has_focus;
  mutable dual_ptr<animation::Approach> hover_animation;

  TextField(std::string *text, float width) : text(text), width(width) {}
  void OnHover(bool hover, animation::State &animation_state) override;
  void OnFocus(bool focus, animation::State &animation_state) override;
  void Draw(SkCanvas &, animation::State &animation_state) const override;
  SkPath Shape() const override;
  std::unique_ptr<Action> KeyDownAction(Key) override;
  bool CanFocusKeyboard() override { return true; }
};

} // namespace automaton::gui