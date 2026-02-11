// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "base.hh"
#include "keyboard.hh"
#include "parent_ref.hh"

namespace automat::library {

struct KeyPresser : Object, ui::Keylogger {
  ui::AnsiKey key = ui::AnsiKey::F;

  ui::Keylogging* keylogging = nullptr;
  bool key_pressed = false;

  struct Monitoring : OnOff {
    StrView Name() const override { return "Monitoring"sv; }
    bool IsOn() const override;
    void OnTurnOn() override;
    void OnTurnOff() override;
    PARENT_REF(KeyPresser, monitoring)
  } monitoring;

  struct State : OnOff {
    StrView Name() const override { return "State"sv; }
    bool IsOn() const override { return KeyPresser().key_pressed; }
    void OnTurnOn() override;
    void OnTurnOff() override;

    void OnSync() override;
    void OnUnsync() override;
    PARENT_REF(KeyPresser, state)
  } state;

  struct Run : Runnable{virtual void OnRun(std::unique_ptr<RunTask> & run_task)
                            override{KeyPresser().state.OnTurnOn();
}

PARENT_REF(KeyPresser, run)
}  // namespace automat::library
run;

KeyPresser(ui::AnsiKey = ui::AnsiKey::F);
~KeyPresser() override;
string_view Name() const override;
Ptr<Object> Clone() const override;

std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;

void SetKey(ui::AnsiKey);

void Atoms(const std::function<LoopControl(Atom&)>& cb) override {
  if (LoopControl::Break == cb(monitoring)) return;
  if (LoopControl::Break == cb(run)) return;
}

OnOff* AsOnOff() override { return &state; }

void SerializeState(ObjectSerializer& writer) const override;
bool DeserializeKey(ObjectDeserializer& d, StrView key) override;

void KeyloggerKeyDown(ui::Key) override;
void KeyloggerKeyUp(ui::Key) override;
void KeyloggerOnRelease(const ui::Keylogging&) override;
}
;

}  // namespace automat::library
