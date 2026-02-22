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

  DEF_INTERFACE(KeyPresser, OnOff, monitoring, "Monitoring")
  bool IsOn() const { return obj->keylogging != nullptr; }
  void OnTurnOn() { obj->StartMonitoring(); }
  void OnTurnOff() { obj->StopMonitoring(); }
  DEF_END(monitoring);

  DEF_INTERFACE(KeyPresser, OnOff, state, "State")
  bool IsOn() const { return obj->key_pressed; }
  void OnTurnOn() { obj->Press(); }
  void OnTurnOff() { obj->Release(); }
  void OnSync() { obj->monitoring->TurnOn(); }
  void OnUnsync() { obj->monitoring->TurnOff(); }
  DEF_END(state);

  DEF_INTERFACE(KeyPresser, Runnable, run, "Run")
  void OnRun(std::unique_ptr<RunTask>&) { obj->state->TurnOn(); }
  DEF_END(run);

  KeyPresser(ui::AnsiKey = ui::AnsiKey::F);
  ~KeyPresser() override;
  string_view Name() const override;
  Ptr<Object> Clone() const override;
  void StartMonitoring();
  void StopMonitoring();
  void Press();
  void Release();

  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;

  void SetKey(ui::AnsiKey);

  INTERFACES(state, monitoring, run)

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;

  void KeyloggerKeyDown(ui::Key) override;
  void KeyloggerKeyUp(ui::Key) override;
  void KeyloggerOnRelease(const ui::Keylogging&) override;
};

}  // namespace automat::library
