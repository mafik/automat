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

  struct Enabled : OnOff {
    using Parent = HotKey;
    static constexpr StrView kName = "Enabled"sv;
    static constexpr int Offset() { return offsetof(HotKey, enabled); }

    bool IsOn() const { return object().hotkey != nullptr; }
    void OnTurnOn();
    void OnTurnOff();
  };
  OnOff::Def<Enabled> enabled;

  struct Next : NextArg {
    using Parent = HotKey;
    static constexpr StrView kName = "Next"sv;
    static constexpr int Offset() { return offsetof(HotKey, next); }
  };
  NextArg::Def<Next> next;

  HotKey();
  HotKey(const HotKey&);
  string_view Name() const override;
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
