// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "base.hh"
#include "keyboard.hh"

namespace automat::library {

struct HotKey : Object, SignalNext, ui::KeyGrabber {
  ui::AnsiKey key = ui::AnsiKey::F11;
  bool ctrl = true;
  bool alt = false;
  bool shift = false;
  bool windows = false;

  // This is used to get hotkey events
  ui::KeyGrab* hotkey = nullptr;

  struct Enabled : OnOff {
    StrView Name() const override { return "Enabled"sv; }
    bool IsOn() const override;
    void OnTurnOn() override;
    void OnTurnOff() override;

    HotKey& GetHotKey() const {
      return *reinterpret_cast<HotKey*>(reinterpret_cast<intptr_t>(this) -
                                        offsetof(HotKey, enabled));
    }
  } enabled;

  HotKey();
  HotKey(const HotKey&);
  string_view Name() const override;
  Ptr<Object> Clone() const override;
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;
  void Atoms(const std::function<void(Atom&)>& cb) override;

  void ReleaseKeyGrab(ui::KeyGrab&) override;
  void KeyGrabberKeyDown(ui::KeyGrab&) override;
  void KeyGrabberKeyUp(ui::KeyGrab&) override;

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;
};

}  // namespace automat::library
