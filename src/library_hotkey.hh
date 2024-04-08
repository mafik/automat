#pragma once

#include <memory>

#include "base.hh"
#include "keyboard.hh"

namespace automat::library {

struct OnOff {
  virtual ~OnOff() = default;

  virtual bool IsOn() const = 0;
  virtual void On() = 0;
  virtual void Off() = 0;

  void Toggle() {
    if (IsOn())
      Off();
    else
      On();
  }
};

struct PowerButton : gui::ToggleButton {
  OnOff* target;
  PowerButton(OnOff* target);
  void Activate(gui::Pointer&) override { target->Toggle(); }
  bool Filled() const override { return target->IsOn(); }
};

struct KeyButton : gui::Button {
  float width;
  function<void(gui::Pointer&)> activate;
  KeyButton(std::unique_ptr<Widget> child, SkColor color, float width);
  void Activate(gui::Pointer&) override;
  SkRRect RRect() const override;
  void DrawButtonFace(gui::DrawContext&, SkColor bg, SkColor fg) const override;
};

struct HotKey : LiveObject, OnOff, gui::KeyboardGrabber, gui::KeyGrabber {
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

  // This is used to select the main hotkey
  gui::KeyboardGrab* hotkey_selector = nullptr;

  // This is used to get hotkey events
  gui::KeyGrab* hotkey = nullptr;

  HotKey();
  string_view Name() const override;
  std::unique_ptr<Object> Clone() const override;
  void Draw(gui::DrawContext&) const override;
  SkPath Shape() const override;
  void Args(std::function<void(Argument&)> cb) override;
  void Run(Location&) override;

  bool IsOn() const override;
  void On() override;
  void Off() override;

  void ReleaseGrab(gui::KeyboardGrab&) override;
  void ReleaseKeyGrab(gui::KeyGrab&) override;
  Widget* GrabWidget() override { return this; }

  void KeyboardGrabberKeyDown(gui::KeyboardGrab&, gui::Key) override;

  void KeyGrabberKeyDown(gui::KeyGrab&) override;
  void KeyGrabberKeyUp(gui::KeyGrab&) override;

  ControlFlow VisitChildren(gui::Visitor& visitor) override;

  SkMatrix TransformToChild(const Widget& child, animation::Context&) const override;
};

}  // namespace automat::library