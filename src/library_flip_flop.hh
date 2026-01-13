// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "base.hh"
#include "widget.hh"

namespace automat::library {

struct FlipFlop : Object, Runnable, OnOff {
  bool current_state = false;

  FlipFlop();
  string_view Name() const override;
  Ptr<Object> Clone() const override;
  operator OnOff*() override { return this; }

  void SetKey(ui::AnsiKey);

  void OnRun(std::unique_ptr<RunTask>&) override;
  void SerializeState(ObjectSerializer& writer) const override;
  void DeserializeState(ObjectDeserializer& d) override;

  bool IsOn() const override { return current_state; }
  void OnTurnOn() override;
  void OnTurnOff() override;

  std::unique_ptr<ObjectWidget> MakeWidget(ui::Widget* parent) override;
};

}  // namespace automat::library
