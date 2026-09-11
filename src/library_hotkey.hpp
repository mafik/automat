#pragma once
// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT

#include "base.hpp"
#include "keyboard.hpp"

namespace automat::library {

struct HotKey : Object, ui::KeyGrabber {
  ui::AnsiKey key = ui::AnsiKey::F11;
  bool ctrl = true;
  bool alt = false;
  bool shift = false;
  bool windows = false;

  // This is used to get hotkey events
  MortalPtr<ui::KeyGrab> hotkey;

  DEF_INTERFACE(HotKey, OnOff, enabled, "Enabled")
  bool IsOn() const { return obj->hotkey != nullptr; }
  void OnTurnOn() { obj->Enable(); }
  void OnTurnOff() { obj->Disable(); }
  DEF_END(enabled);

  DEF_INTERFACE(HotKey, NextArg, next, "Next")
  DEF_END(next);

  DEF_INTERFACE(HotKey, Signal, toggle_ctrl, "Ctrl")
  static constexpr bool kSchedulesNext = false;
  void OnRun(std::unique_ptr<RunTask>&) { obj->ToggleModifier(obj->ctrl); }
  DEF_END(toggle_ctrl);

  DEF_INTERFACE(HotKey, Signal, toggle_alt, "Alt")
  static constexpr bool kSchedulesNext = false;
  void OnRun(std::unique_ptr<RunTask>&) { obj->ToggleModifier(obj->alt); }
  DEF_END(toggle_alt);

  DEF_INTERFACE(HotKey, Signal, toggle_shift, "Shift")
  static constexpr bool kSchedulesNext = false;
  void OnRun(std::unique_ptr<RunTask>&) { obj->ToggleModifier(obj->shift); }
  DEF_END(toggle_shift);

  DEF_INTERFACE(HotKey, Signal, toggle_super, "Super")
  static constexpr bool kSchedulesNext = false;
  void OnRun(std::unique_ptr<RunTask>&) { obj->ToggleModifier(obj->windows); }
  DEF_END(toggle_super);

  HotKey();
  HotKey(const HotKey&);
  string_view Name() const override;
  void Enable();
  void Disable();
  void ToggleModifier(bool& modifier);
  Ptr<Object> Clone() const override;
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;
  INTERFACES(next, enabled, toggle_ctrl, toggle_alt, toggle_shift, toggle_super)

  void ReleaseKeyGrab(ui::KeyGrab&) override;
  void KeyGrabberKeyDown(ui::KeyGrab&) override;
  void KeyGrabberKeyUp(ui::KeyGrab&) override;

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;
};

}  // namespace automat::library
