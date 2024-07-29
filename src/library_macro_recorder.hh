#pragma once

#include <memory>

#include "animation.hh"
#include "base.hh"
#include "color.hh"
#include "keyboard.hh"
#include "on_off.hh"
#include "run_button.hh"

namespace automat::library {

struct GlassRunButton : gui::PowerButton {
  GlassRunButton(OnOff* on_off) : gui::PowerButton(on_off) {}
  SkColor ForegroundColor(gui::DrawContext&) const override { return color::kParrotRed; }
  void PointerOver(gui::Pointer&, animation::Display&) override;
  void PointerLeave(gui::Pointer&, animation::Display&) override;
  // SkColor BackgroundColor() const override { return "#490b13"_color; }
};

struct MacroRecorder : LiveObject, Runnable, LongRunning, gui::Keylogger, OnOff {
  static const MacroRecorder proto;

  struct AnimationState {
    animation::Spring<Vec2> googly_left;
    animation::Spring<Vec2> googly_right;
    animation::Approach<> eye_speed;
    float eye_rotation = 0;
    int pointers_over = 0;
    animation::Approach<> eyes_open;
  };

  animation::PerDisplay<AnimationState> animation_state_ptr;
  gui::Keylogging* keylogging = nullptr;
  GlassRunButton record_button;

  MacroRecorder();
  ~MacroRecorder();
  string_view Name() const override;
  std::unique_ptr<Object> Clone() const override;
  void Draw(gui::DrawContext&) const override;
  SkPath Shape(animation::Display*) const override;
  ControlFlow VisitChildren(gui::Visitor& visitor) override {
    Widget* widgets[] = {&record_button};
    if (visitor(widgets) == ControlFlow::Stop) return ControlFlow::Stop;
    return ControlFlow::Continue;
  }

  void Args(std::function<void(Argument&)> cb) override;
  const Object* ArgPrototype(const Argument&) override;

  bool IsOn() const override;
  void On() override;
  void Off() override;

  void PointerOver(gui::Pointer&, animation::Display&) override;
  void PointerLeave(gui::Pointer&, animation::Display&) override;

  SkMatrix TransformToChild(const Widget& child, animation::Display*) const override;

  LongRunning* OnRun(Location& here) override;
  void Cancel() override;
  void KeyloggerKeyDown(gui::Key) override;
  void KeyloggerKeyUp(gui::Key) override;
};

}  // namespace automat::library