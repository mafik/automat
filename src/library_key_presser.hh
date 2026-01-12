// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "base.hh"
#include "keyboard.hh"

namespace automat::library {

struct KeyPresser : Object, OnOff, ui::Keylogger {
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
  };

  Monitoring monitoring;

  KeyPresser(ui::AnsiKey = ui::AnsiKey::F);
  ~KeyPresser() override;
  string_view Name() const override;
  Ptr<Object> Clone() const override;

  std::unique_ptr<ObjectWidget> MakeWidget(ui::Widget* parent) override;

  void SetKey(ui::AnsiKey);

  void Parts(const std::function<void(Part&)>& cb) override {
    cb(monitoring);
    cb(*this);
  }

  void PartName(Part& part, Str& out_name) override {
    if (&part == this) {
      out_name = "";
      return;
    }
    return Object::PartName(part, out_name);
  }

  operator OnOff*() override { return this; }
  bool IsOn() const override { return key_pressed; }
  void OnTurnOn() override;
  void OnTurnOff() override;

  void OnSync() override;
  void OnUnsync() override;

  void SerializeState(ObjectSerializer& writer, const char* key) const override;
  void DeserializeState(ObjectDeserializer& d) override;

  void KeyloggerKeyDown(ui::Key) override;
  void KeyloggerKeyUp(ui::Key) override;
  void KeyloggerOnRelease(const ui::Keylogging&) override;
};

}  // namespace automat::library
