#pragma once

#include "base.hh"
#include "key_button.hh"

namespace automat::library {

struct KeyPresser;

struct KeyPresserButton : KeyButton {
  KeyPresser* key_presser;
  using KeyButton::KeyButton;
  float PressRatio() const override;
};

struct KeyPresser : Object, gui::KeyboardGrabber, Runnable, LongRunning {
  static const KeyPresser proto;

  gui::AnsiKey key = gui::AnsiKey::F;

  mutable KeyPresserButton shortcut_button;

  // This is used to select the pressed key
  gui::KeyboardGrab* key_selector = nullptr;
  bool key_pressed = false;

  KeyPresser(gui::AnsiKey);
  KeyPresser();
  string_view Name() const override;
  std::unique_ptr<Object> Clone() const override;
  void Draw(gui::DrawContext&) const override;
  SkPath Shape() const override;
  void ConnectionPositions(maf::Vec<Vec2AndDir>& out_positions) const override;
  std::unique_ptr<Action> CaptureButtonDownAction(gui::Pointer& p, gui::PointerButton btn) override;

  void KeyboardGrabberKeyDown(gui::KeyboardGrab&, gui::Key) override;
  void ReleaseGrab(gui::KeyboardGrab&) override;
  Widget* GrabWidget() override { return this; }

  void SetKey(gui::AnsiKey);

  ControlFlow VisitChildren(gui::Visitor& visitor) override;
  SkMatrix TransformToChild(const Widget& child, animation::Display*) const override;

  LongRunning* OnRun(Location& here) override;
  void Cancel() override;
};

}  // namespace automat::library