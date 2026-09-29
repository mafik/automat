#pragma once
// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT

#include "base.hpp"
#include "keyboard.hpp"

namespace automat::library {

extern constinit Command::Table kSelectHotKey;

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
  static constexpr int TurnOnIdx() { return SlotOf(enable_tbl); }
  static constexpr int TurnOffIdx() { return SlotOf(disable_tbl); }
  DEF_END(enabled);

  DEF_INTERFACE(HotKey, Command, enable, "Enable")
  void OnRun() { obj->Enable(); }
  DEF_END(enable);

  DEF_INTERFACE(HotKey, Command, disable, "Disable")
  void OnRun() { obj->Disable(); }
  DEF_END(disable);

  DEF_INTERFACE(HotKey, NextArg, next, "Next")
  DEF_END(next);

  DEF_INTERFACE(HotKey, Command, toggle_ctrl, "Ctrl")
  void OnRun() { obj->ToggleModifier(obj->ctrl); }
  DEF_END(toggle_ctrl);

  DEF_INTERFACE(HotKey, Command, toggle_alt, "Alt")
  void OnRun() { obj->ToggleModifier(obj->alt); }
  DEF_END(toggle_alt);

  DEF_INTERFACE(HotKey, Command, toggle_shift, "Shift")
  void OnRun() { obj->ToggleModifier(obj->shift); }
  DEF_END(toggle_shift);

  DEF_INTERFACE(HotKey, Command, toggle_super, "Super")
  void OnRun() { obj->ToggleModifier(obj->windows); }
  DEF_END(toggle_super);

  HotKey();
  HotKey(const HotKey&);
  string_view Name() const override;
  void Enable();
  void Disable();
  void ToggleModifier(bool& modifier);
  Ptr<Object> Clone() const override;
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;
  INTERFACES(next_tbl, enabled_tbl, enable_tbl, disable_tbl, toggle_ctrl_tbl, toggle_alt_tbl,
             toggle_shift_tbl, toggle_super_tbl, kSelectHotKey);

  void ReleaseKeyGrab(ui::KeyGrab&) override;
  void KeyGrabberKeyDown(ui::KeyGrab&) override;
  void KeyGrabberKeyUp(ui::KeyGrab&) override;

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;
};

}  // namespace automat::library
