// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "base.hh"
#include "parent_ref.hh"
#include "widget.hh"

namespace automat::library {

struct FlipFlop : Object {
  bool current_state = false;

  struct Flip : Runnable {
    void OnRun(std::unique_ptr<RunTask>&) override;

    PARENT_REF(FlipFlop, flip)
  } flip;

  struct State : OnOff {
    bool IsOn() const override { return FlipFlop().current_state; }
    void OnTurnOn() override;
    void OnTurnOff() override;

    PARENT_REF(FlipFlop, on_off)
  } on_off;

  FlipFlop();
  ~FlipFlop();
  string_view Name() const override;
  Ptr<Object> Clone() const override;
  void SetKey(ui::AnsiKey);

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;

  void Interfaces(const std::function<LoopControl(Interface&)>& cb) override {
    if (LoopControl::Break == cb(flip)) return;
    if (LoopControl::Break == cb(on_off)) return;
  }

  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;
};

}  // namespace automat::library
