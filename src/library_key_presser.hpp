#pragma once
// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT

#include "base.hpp"
#include "keyboard.hpp"
#include "syncable.hpp"
#include "time.hpp"

namespace automat::library {

struct KeyPresser : Object, ui::Keylogger {
  ui::AnsiKey key = ui::AnsiKey::F;

  MortalPtr<ui::Keylogging> keylogging;
  time::SteadyPoint last_pressed_time = time::kZeroSteady;
  time::SteadyPoint last_released_time = time::kZeroSteady;
  bool key_pressed = false;

  DEF_INTERFACE(KeyPresser, OnOff, monitoring, "Monitoring")
  bool IsOn() const { return obj->keylogging != nullptr; }
  static constexpr int TurnOnIdx() { return SlotOf(start_monitoring_tbl); }
  static constexpr int TurnOffIdx() { return SlotOf(stop_monitoring_tbl); }
  DEF_END(monitoring);

  DEF_INTERFACE(KeyPresser, Command, start_monitoring, "Start monitoring")
  void OnRun() { obj->StartMonitoring(); }
  DEF_END(start_monitoring);

  DEF_INTERFACE(KeyPresser, Command, stop_monitoring, "Stop monitoring")
  void OnRun() { obj->StopMonitoring(); }
  DEF_END(stop_monitoring);

  DEF_INTERFACE(KeyPresser, OnOff, state, "State")
  bool IsOn() const { return obj->key_pressed; }
  static constexpr int TurnOnIdx() { return SlotOf(press_tbl); }
  static constexpr int TurnOffIdx() { return SlotOf(release_tbl); }
  static constexpr int SyncableIdx() { return SlotOf(syncable_tbl); }
  DEF_END(state);

  DEF_INTERFACE(KeyPresser, Syncable, syncable, "Syncable")
  void OnSync() {
    // Prevent monitoring from being started while object is being deserialized.
    if (obj->suspended) {
      return;
    }
    obj->monitoring->TurnOn();
  }
  void OnUnsync() {
    if (obj->suspended) {
      return;
    }
    obj->monitoring->TurnOff();
  }
  DEF_END(syncable);

  DEF_INTERFACE(KeyPresser, Command, press, "Press")
  void OnRun() { obj->Press(); }
  DEF_END(press);

  DEF_INTERFACE(KeyPresser, Command, release, "Release")
  void OnRun() { obj->Release(); }
  DEF_END(release);

  DEF_INTERFACE(KeyPresser, Command, run, "Run")
  void OnRun() { obj->state->TurnOn(); }
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

  INTERFACES(run_tbl, state_tbl, press_tbl, release_tbl, monitoring_tbl, start_monitoring_tbl,
             stop_monitoring_tbl, syncable_tbl);

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;

  void KeyloggerKeyDown(ui::Key) override;
  void KeyloggerKeyUp(ui::Key) override;
  void KeyloggerOnRelease(const ui::Keylogging&) override;
};

}  // namespace automat::library
