#pragma once

#include "animation.hh"
#include "base.hh"
#include "gui_button.hh"

namespace automat::library {

struct FlipFlop;

struct YingYangIcon : gui::Widget, gui::PaintMixin {
  YingYangIcon() = default;
  void Draw(gui::DrawContext&) const override;
  SkPath Shape(animation::Display*) const override;
};

struct FlipFlopButton : gui::ToggleButton {
  FlipFlop* flip_flop;
  mutable YingYangIcon icon;
  using ToggleButton::ToggleButton;
  bool Filled() const override;
  SkRRect RRect() const override;
  Widget* Child() const override { return &icon; }
  void Activate(gui::Pointer&) override;
  void TweakShadow(float& sigma, float& offset) const override;
  SkColor ForegroundColor(gui::DrawContext&) const override;
  SkColor BackgroundColor() const override;
};

struct FlipFlop : LiveObject, Runnable {
  static const FlipFlop proto;

  mutable FlipFlopButton button;

  bool current_state = false;
  struct AnimationState {
    animation::Approach<float> light;
  };
  animation::PerDisplay<AnimationState> animation_states;

  FlipFlop();
  string_view Name() const override;
  std::unique_ptr<Object> Clone() const override;
  void Draw(gui::DrawContext&) const override;
  SkPath Shape(animation::Display*) const override;
  void Args(std::function<void(Argument&)> cb) override;

  void SetKey(gui::AnsiKey);

  ControlFlow VisitChildren(gui::Visitor& visitor) override;
  SkMatrix TransformToChild(const Widget& child, animation::Display*) const override;

  LongRunning* OnRun(Location& here) override;
  void SerializeState(Serializer& writer, const char* key) const override;
  void DeserializeState(Location& l, Deserializer& d) override;
};

}  // namespace automat::library