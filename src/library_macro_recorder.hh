#pragma once

#include "animation.hh"
#include "base.hh"
#include "product_ptr.hh"

namespace automat::library {

struct MacroRecorder : Object, Runnable, LongRunning {
  static const MacroRecorder proto;

  struct AnimationState {
    animation::Spring<Vec2> googly_left;
    animation::Spring<Vec2> googly_right;
  };

  mutable product_ptr<AnimationState> animation_state_ptr;

  MacroRecorder();
  string_view Name() const override;
  std::unique_ptr<Object> Clone() const override;
  void Draw(gui::DrawContext&) const override;
  SkPath Shape() const override;

  LongRunning* OnRun(Location& here) override;
  void Cancel() override;
};

}  // namespace automat::library