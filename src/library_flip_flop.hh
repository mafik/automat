// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "animation.hh"
#include "base.hh"
#include "ui_button.hh"
#include "widget.hh"

namespace automat::library {

struct FlipFlop : LiveObject, Runnable, OnOff {
  bool current_state = false;

  FlipFlop();
  string_view Name() const override;
  Ptr<Object> Clone() const override;
  operator OnOff*() override { return this; }

  void SetKey(ui::AnsiKey);

  void OnRun(Location& here, std::unique_ptr<RunTask>&) override;
  void SerializeState(Serializer& writer, const char* key) const override;
  void DeserializeState(Deserializer& d) override;

  bool IsOn() const override { return current_state; }
  void OnTurnOn() override;
  void OnTurnOff() override;

  std::unique_ptr<ObjectWidget> MakeWidget(ui::Widget* parent) override;
};

}  // namespace automat::library
