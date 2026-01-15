// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "base.hh"
#include "key_button.hh"
#include "keyboard.hh"
#include "sync.hh"

namespace automat::library {

struct HotKey : Object, Object::WidgetBase, OnOff, SignalNext, ui::CaretOwner, ui::KeyGrabber {
  ui::AnsiKey key = ui::AnsiKey::F11;
  bool ctrl = true;
  bool alt = false;
  bool shift = false;
  bool windows = false;

  unique_ptr<ui::PowerButton> power_button;
  unique_ptr<KeyButton> ctrl_button;
  unique_ptr<KeyButton> alt_button;
  unique_ptr<KeyButton> shift_button;
  unique_ptr<KeyButton> windows_button;
  unique_ptr<KeyButton> shortcut_button;

  // This is used to select the main hotkey
  ui::Caret* hotkey_selector = nullptr;

  // This is used to get hotkey events
  ui::KeyGrab* hotkey = nullptr;

  HotKey(ui::Widget* parent);
  string_view Name() const override;
  Ptr<Object> Clone() const override;
  animation::Phase Tick(time::Timer&) override;
  void Draw(SkCanvas&) const override;
  SkPath Shape() const override;
  bool CenteredAtZero() const override { return true; }
  void Parts(const std::function<void(Part&)>& cb) override;

  bool IsOn() const override;
  void OnTurnOn() override;
  void OnTurnOff() override;

  void ReleaseCaret(ui::Caret&) override;
  void ReleaseKeyGrab(ui::KeyGrab&) override;

  void KeyDown(ui::Caret&, ui::Key) override;

  void KeyGrabberKeyDown(ui::KeyGrab&) override;
  void KeyGrabberKeyUp(ui::KeyGrab&) override;

  void FillChildren(Vec<Widget*>& children) override;

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;
};

}  // namespace automat::library
