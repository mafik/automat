// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "base.hh"
#include "widget.hh"

namespace automat::library {

struct FlipFlop : Object {
  bool current_state = false;

  DEF_INTERFACE(FlipFlop, Runnable, flip, "Flip")
    void OnRun(std::unique_ptr<RunTask>&) { obj->enabled->Toggle(); }
  DEF_END(flip);

  DEF_INTERFACE(FlipFlop, OnOff, enabled, "State")
    bool IsOn() const { return obj->current_state; }
    void OnTurnOn() {
      obj->current_state = true;
      obj->WakeToys();
    }
    void OnTurnOff() {
      obj->current_state = false;
      obj->WakeToys();
    }
  DEF_END(enabled);

  INTERFACES(flip, enabled)

  FlipFlop() = default;
  string_view Name() const override;
  Ptr<Object> Clone() const override;
  void SetKey(ui::AnsiKey);

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;

  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;
};

}  // namespace automat::library
