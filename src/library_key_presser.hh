// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "base.hh"
#include "keyboard.hh"

namespace automat::library {

struct KeyPresser : LiveObject, Runnable, LongRunning, ui::Keylogger {
  ui::AnsiKey key = ui::AnsiKey::F;

  ui::Keylogging* keylogging = nullptr;
  bool key_pressed = false;

  struct Monitoring : OnOff {
    StrView Name() const override { return "Monitoring"sv; }
    bool IsOn() const override;
    void OnTurnOn() override;
    void OnTurnOff() override;
    // operator OnOff*() override { return this; }
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
  void Args(std::function<void(Argument&)> cb) override;
  void OnRun(Location& here, std::unique_ptr<RunTask>&) override;
  void OnCancel() override;
  LongRunning* AsLongRunning() override { return this; }

  void SerializeState(Serializer& writer, const char* key) const override;
  void DeserializeState(Location& l, Deserializer& d) override;

  void KeyloggerKeyDown(ui::Key) override;
  void KeyloggerKeyUp(ui::Key) override;
  void KeyloggerOnRelease(const ui::Keylogging&) override;
};

}  // namespace automat::library
