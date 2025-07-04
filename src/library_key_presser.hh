// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "base.hh"
#include "key_button.hh"

namespace automat::library {

struct KeyPresser;

struct KeyPresserButton : KeyButton {
  KeyPresser* key_presser;
  KeyPresserButton(KeyPresser* key_presser, Ptr<Widget> parent, SkColor color, float width)
      : key_presser(key_presser), KeyButton(parent, color, width) {}
  using KeyButton::KeyButton;
  float PressRatio() const override;
};

struct KeyPresser : Object, Object::FallbackWidget, gui::CaretOwner, Runnable, LongRunning {
  gui::AnsiKey key = gui::AnsiKey::F;

  mutable Ptr<KeyPresserButton> shortcut_button;

  // This is used to select the pressed key
  gui::Caret* key_selector = nullptr;
  bool key_pressed = false;

  KeyPresser(gui::AnsiKey);
  KeyPresser();
  ~KeyPresser() override;
  string_view Name() const override;
  Ptr<Object> Clone() const override;
  animation::Phase Tick(time::Timer&) override;
  void Draw(SkCanvas&) const override;
  SkPath Shape() const override;
  void ConnectionPositions(Vec<Vec2AndDir>& out_positions) const override;
  std::unique_ptr<Action> FindAction(gui::Pointer& p, gui::ActionTrigger btn) override;

  void KeyDown(gui::Caret&, gui::Key) override;
  void ReleaseCaret(gui::Caret&) override;

  void SetKey(gui::AnsiKey);

  void FillChildren(Vec<Ptr<Widget>>& children) override;
  bool AllowChildPointerEvents(Widget& child) const override { return false; }

  void OnRun(Location& here, RunTask& run_task) override;
  void OnCancel() override;
  LongRunning* AsLongRunning() override { return this; }

  void SerializeState(Serializer& writer, const char* key) const override;
  void DeserializeState(Location& l, Deserializer& d) override;
};

}  // namespace automat::library