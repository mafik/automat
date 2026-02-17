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

  struct Monitoring : OnOff {
    using Parent = KeyPresser;
    static constexpr StrView kName = "Monitoring"sv;
    static constexpr int Offset() { return offsetof(KeyPresser, monitoring); }

    bool IsOn() const { return self().keylogging != nullptr; }
    void OnTurnOn();
    void OnTurnOff();
  };
  OnOff::Def<Monitoring> monitoring;

  struct State : OnOff {
    using Parent = KeyPresser;
    static constexpr StrView kName = "State"sv;
    static constexpr int Offset() { return offsetof(KeyPresser, state); }

    bool IsOn() const { return self().key_pressed; }
    void OnTurnOn();
    void OnTurnOff();
    void OnSync();
    void OnUnsync();
  };
  OnOff::Def<State> state;

  struct RunImpl : Runnable {
    using Parent = KeyPresser;
    static constexpr StrView kName = "Run"sv;
    static constexpr int Offset() { return offsetof(KeyPresser, run); }
    void OnRun(std::unique_ptr<RunTask>&);
  };
  Runnable::Def<RunImpl> run;

  KeyPresser(ui::AnsiKey = ui::AnsiKey::F);
  ~KeyPresser() override;
  string_view Name() const override;
  Ptr<Object> Clone() const override;

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
