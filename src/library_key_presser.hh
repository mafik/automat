// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "base.hh"
#include "keyboard.hh"

namespace automat::library {

struct KeyPresser : LiveObject, OnOff, ui::Keylogger {
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

  Interface* interfaces[1] = {&monitoring};

  KeyPresser(ui::AnsiKey = ui::AnsiKey::F);
  ~KeyPresser() override;
  string_view Name() const override;
  Ptr<Object> Clone() const override;

  std::unique_ptr<WidgetInterface> MakeWidget(ui::Widget* parent) override;

  void SetKey(ui::AnsiKey);

  Span<Interface*> Interfaces() override { return interfaces; }

  operator OnOff*() override { return this; }
  bool IsOn() const override { return key_pressed; }
  void OnTurnOn() override;
  void OnTurnOff() override;

  void OnSync() override;
  void OnUnsync() override;

  void SerializeState(Serializer& writer, const char* key) const override;
  void DeserializeState(Location& l, Deserializer& d) override;

  void KeyloggerKeyDown(ui::Key) override;
  void KeyloggerKeyUp(ui::Key) override;
  void KeyloggerOnRelease(const ui::Keylogging&) override;
};

}  // namespace automat::library
