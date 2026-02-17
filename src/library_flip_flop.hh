// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "base.hh"
#include "widget.hh"

namespace automat::library {

struct FlipFlop : Object {
  bool current_state = false;

  struct Flip : Runnable {
    using Parent = FlipFlop;
    static constexpr StrView kName = "Flip"sv;
    static constexpr int Offset() { return offsetof(FlipFlop, flip); }
    void OnRun(std::unique_ptr<RunTask>&);
  };
  Runnable::Def<Flip> flip;

  struct Enabled : OnOff {
    using Parent = FlipFlop;
    static constexpr StrView kName = "State"sv;
    static constexpr int Offset() { return offsetof(FlipFlop, enabled); }
    bool IsOn() const { return self().current_state; }
    void OnTurnOn() {
      self().current_state = true;
      self().WakeToys();
    }
    void OnTurnOff() {
      self().current_state = false;
      self().WakeToys();
    }
  };
  OnOff::Def<Enabled> enabled;

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
