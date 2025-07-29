// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "base.hh"
#include "key_button.hh"

namespace automat::library {

struct KeyPresser;

struct KeyPresserButton : KeyButton {
  KeyPresser* key_presser;
  KeyPresserButton(Widget* parent, KeyPresser* key_presser, StrView label, SkColor color,
                   float width)
      : key_presser(key_presser), KeyButton(parent, label, color, width) {}
  using KeyButton::KeyButton;
  float PressRatio() const override;
};

struct KeyPresser : Object, Object::FallbackWidget, ui::CaretOwner, Runnable, LongRunning {
  ui::AnsiKey key = ui::AnsiKey::F;

  mutable std::unique_ptr<KeyPresserButton> shortcut_button;

  // This is used to select the pressed key
  ui::Caret* key_selector = nullptr;
  bool key_pressed = false;

  KeyPresser(ui::Widget* parent, ui::AnsiKey);
  KeyPresser(ui::Widget* parent);
  ~KeyPresser() override;
  string_view Name() const override;
  Ptr<Object> Clone() const override;
  animation::Phase Tick(time::Timer&) override;
  void Draw(SkCanvas&) const override;
  SkPath Shape() const override;
  void ConnectionPositions(Vec<Vec2AndDir>& out_positions) const override;
  std::unique_ptr<Action> FindAction(ui::Pointer& p, ui::ActionTrigger btn) override;

  void KeyDown(ui::Caret&, ui::Key) override;
  void ReleaseCaret(ui::Caret&) override;

  void SetKey(ui::AnsiKey);

  void FillChildren(Vec<Widget*>& children) override;
  bool AllowChildPointerEvents(Widget& child) const override { return false; }

  void OnRun(Location& here, RunTask& run_task) override;
  void OnCancel() override;
  LongRunning* AsLongRunning() override { return this; }

  void SerializeState(Serializer& writer, const char* key) const override;
  void DeserializeState(Location& l, Deserializer& d) override;
};

}  // namespace automat::library