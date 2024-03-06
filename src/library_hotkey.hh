#pragma once

#include <memory>

#include "base.hh"
#include "keyboard.hh"

#if defined(__linux__)
#include "linux_main.hh"
#endif

namespace automat::library {

struct OnOff {
  bool on;
  virtual ~OnOff() = default;

  virtual void Toggle() {
    on = !on;
    on ? On() : Off();
  }
  virtual void On() {};
  virtual void Off() {};
};

struct PowerButton : gui::ToggleButton {
  OnOff* target;
  PowerButton(OnOff* target);
  void Activate(gui::Pointer&) override { target->Toggle(); }
  bool Filled() const override { return target->on; }
};

struct KeyButton : gui::Button {
  float width;
  function<void(gui::Pointer&)> activate;
  KeyButton(std::unique_ptr<Widget> child, SkColor color, float width);
  void Activate(gui::Pointer&) override;
  SkRRect RRect() const override;
  void DrawButtonFace(gui::DrawContext&, SkColor bg, SkColor fg) const override;
};

struct HotKey : Object, OnOff, SystemEventHook {
  static const HotKey proto;

  gui::AnsiKey key = gui::AnsiKey::F11;
  bool ctrl = true;
  bool alt = false;
  bool shift = false;
  bool windows = false;
  bool active = false;

  PowerButton power_button;

  KeyButton ctrl_button;
  KeyButton alt_button;
  KeyButton shift_button;
  KeyButton windows_button;
  mutable KeyButton shortcut_button;

  bool recording = false;

  HotKey();
  string_view Name() const override;
  std::unique_ptr<Object> Clone() const override;
  void Draw(gui::DrawContext&) const override;
  SkPath Shape() const override;
  std::unique_ptr<Action> ButtonDownAction(gui::Pointer&, gui::PointerButton) override;
  void Args(std::function<void(Argument&)> cb) override;
  void Run(Location&) override;

  void On() override;
  void Off() override;
  bool Intercept(xcb_generic_event_t*) override;

  ControlFlow VisitChildren(gui::Visitor& visitor) override;

  SkMatrix TransformToChild(const Widget& child, animation::Context&) const override;
};

}  // namespace automat::library