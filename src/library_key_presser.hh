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
    StrView Name() const override { return "Monitoring"sv; }
    bool IsOn() const override;
    void OnTurnOn() override;
    void OnTurnOff() override;
    KeyPresser& GetKeyPresser() const {
      return *reinterpret_cast<KeyPresser*>(reinterpret_cast<intptr_t>(this) -
                                            offsetof(KeyPresser, monitoring));
    }
  } monitoring;

  struct State : OnOff {
    StrView Name() const override { return "State"sv; }
    bool IsOn() const override { return GetKeyPresser().key_pressed; }
    void OnTurnOn() override;
    void OnTurnOff() override;

    void OnSync() override;
    void OnUnsync() override;
    KeyPresser& GetKeyPresser() const {
      return *reinterpret_cast<KeyPresser*>(reinterpret_cast<intptr_t>(this) -
                                            offsetof(KeyPresser, state));
    }
  } state;

  struct Run : Runnable {
    virtual void OnRun(std::unique_ptr<RunTask>& run_task) override {
      GetKeyPresser().state.OnTurnOn();
    }

    KeyPresser& GetKeyPresser() const {
      return *reinterpret_cast<KeyPresser*>(reinterpret_cast<intptr_t>(this) -
                                            offsetof(KeyPresser, run));
    }
  } run;

  KeyPresser(ui::AnsiKey = ui::AnsiKey::F);
  ~KeyPresser() override;
  string_view Name() const override;
  Ptr<Object> Clone() const override;

  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;

  void SetKey(ui::AnsiKey);

  void Parts(const std::function<void(Part&)>& cb) override {
    cb(monitoring);
    cb(run);
    cb(*this);
  }

  void PartName(Part& part, Str& out_name) override {
    if (&part == this) {
      out_name = "";
      return;
    }
    return Object::PartName(part, out_name);
  }

  operator OnOff*() override { return &state; }

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;

  void KeyloggerKeyDown(ui::Key) override;
  void KeyloggerKeyUp(ui::Key) override;
  void KeyloggerOnRelease(const ui::Keylogging&) override;
};

}  // namespace automat::library
