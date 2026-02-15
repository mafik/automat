// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "base.hh"
#include "keyboard.hh"

namespace automat::library {

struct KeyPresser : Object, ui::Keylogger {
  ui::AnsiKey key = ui::AnsiKey::F;

  ui::Keylogging* keylogging = nullptr;
  bool key_pressed = false;

  SyncState monitoring_sync;
  SyncState state_sync;
  SyncState run_sync;

  static OnOff monitoring;
  static OnOff state;
  static Runnable run;

  KeyPresser(ui::AnsiKey = ui::AnsiKey::F);
  ~KeyPresser() override;
  string_view Name() const override;
  Ptr<Object> Clone() const override;

  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;

  void SetKey(ui::AnsiKey);

  void Interfaces(const std::function<LoopControl(Interface&)>& cb) override {
    if (LoopControl::Break == cb(state)) return;
    if (LoopControl::Break == cb(monitoring)) return;
    if (LoopControl::Break == cb(run)) return;
  }

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;

  void KeyloggerKeyDown(ui::Key) override;
  void KeyloggerKeyUp(ui::Key) override;
  void KeyloggerOnRelease(const ui::Keylogging&) override;
};

}  // namespace automat::library
