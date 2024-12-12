// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <memory>

#include "base.hh"
#include "key_button.hh"
#include "keyboard.hh"
#include "on_off.hh"

namespace automat::library {

struct HotKey : LiveObject, Object::FallbackWidget, OnOff, gui::KeyboardGrabber, gui::KeyGrabber {
  gui::AnsiKey key = gui::AnsiKey::F11;
  bool ctrl = true;
  bool alt = false;
  bool shift = false;
  bool windows = false;

  std::shared_ptr<PowerButton> power_button;
  std::shared_ptr<KeyButton> ctrl_button;
  std::shared_ptr<KeyButton> alt_button;
  std::shared_ptr<KeyButton> shift_button;
  std::shared_ptr<KeyButton> windows_button;
  std::shared_ptr<KeyButton> shortcut_button;

  // This is used to select the main hotkey
  gui::KeyboardGrab* hotkey_selector = nullptr;

  // This is used to get hotkey events
  gui::KeyGrab* hotkey = nullptr;

  HotKey();
  string_view Name() const override;
  std::shared_ptr<Object> Clone() const override;
  animation::Phase Tick(time::Timer&) override;
  void Draw(SkCanvas&) const override;
  SkPath Shape() const override;
  void Args(std::function<void(Argument&)> cb) override;

  bool IsOn() const override;
  void On() override;
  void Off() override;

  void ReleaseGrab(gui::KeyboardGrab&) override;
  void ReleaseKeyGrab(gui::KeyGrab&) override;
  Widget* GrabWidget() override { return this; }

  void KeyboardGrabberKeyDown(gui::KeyboardGrab&, gui::Key) override;

  void KeyGrabberKeyDown(gui::KeyGrab&) override;
  void KeyGrabberKeyUp(gui::KeyGrab&) override;

  void FillChildren(maf::Vec<std::shared_ptr<Widget>>& children) override;

  void SerializeState(Serializer& writer, const char* key) const override;
  void DeserializeState(Location& l, Deserializer& d) override;
};

}  // namespace automat::library