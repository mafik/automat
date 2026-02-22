// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "base.hh"
#include "keyboard.hh"

namespace automat::library {

struct HotKey : Object, ui::KeyGrabber {
  ui::AnsiKey key = ui::AnsiKey::F11;
  bool ctrl = true;
  bool alt = false;
  bool shift = false;
  bool windows = false;

  // This is used to get hotkey events
  ui::KeyGrab* hotkey = nullptr;

  DEF_INTERFACE(HotKey, OnOff, enabled, "Enabled")
  bool IsOn() const { return obj->hotkey != nullptr; }
  void OnTurnOn() { obj->Enable(); }
  void OnTurnOff() { obj->Disable(); }
  DEF_END(enabled);

  DEF_INTERFACE(HotKey, NextArg, next, "Next")
  DEF_END(next);

  HotKey();
  HotKey(const HotKey&);
  string_view Name() const override;
  void Enable();
  void Disable();
  Ptr<Object> Clone() const override;
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;
  INTERFACES(next, enabled)

  void ReleaseKeyGrab(ui::KeyGrab&) override;
  void KeyGrabberKeyDown(ui::KeyGrab&) override;
  void KeyGrabberKeyUp(ui::KeyGrab&) override;

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;
};

}  // namespace automat::library
