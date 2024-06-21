#pragma once

#include <memory>

#include "animation.hh"
#include "base.hh"
#include "keyboard.hh"
#include "on_off.hh"
#include "product_ptr.hh"
#include "run_button.hh"
#include "time.hh"

namespace automat::library {

struct GlassRunButton : gui::PowerButton {
  GlassRunButton(OnOff* on_off) : gui::PowerButton(on_off) {}
  SkColor ForegroundColor() const override { return "#bd1929"_color; }
  // SkColor BackgroundColor() const override { return "#490b13"_color; }
};

struct MacroRecorder : LiveObject, Runnable, LongRunning, gui::Keylogger, OnOff {
  static const MacroRecorder proto;

  struct AnimationState {
    animation::Spring<Vec2> googly_left;
    animation::Spring<Vec2> googly_right;
  };

  mutable product_ptr<AnimationState> animation_state_ptr;
  gui::Keylogging* keylogging = nullptr;
  GlassRunButton record_button;
  time::SteadyPoint recording_start_time;

  MacroRecorder();
  ~MacroRecorder();
  string_view Name() const override;
  std::unique_ptr<Object> Clone() const override;
  void Draw(gui::DrawContext&) const override;
  SkPath Shape() const override;
  ControlFlow VisitChildren(gui::Visitor& visitor) override {
    Widget* widgets[] = {&record_button};
    if (visitor(widgets) == ControlFlow::Stop) return ControlFlow::Stop;
    return ControlFlow::Continue;
  }

  bool IsOn() const override;
  void On() override;
  void Off() override;

  SkMatrix TransformToChild(const Widget& child, animation::Context&) const override;

  LongRunning* OnRun(Location& here) override;
  void Cancel() override;
  void KeyloggerKeyDown(gui::Key) override;
  void KeyloggerKeyUp(gui::Key) override;
};

}  // namespace automat::library