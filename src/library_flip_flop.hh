// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "base.hh"
#include "widget.hh"

namespace automat::library {

struct FlipFlop : Object, OnOff {
  bool current_state = false;

  struct Flip : Runnable {
    void OnRun(std::unique_ptr<RunTask>&) override;

    FlipFlop& GetFlipFlop() const {
      return *reinterpret_cast<FlipFlop*>(reinterpret_cast<intptr_t>(this) -
                                          offsetof(FlipFlop, flip));
    }
  } flip;

  FlipFlop();
  ~FlipFlop();
  string_view Name() const override;
  Ptr<Object> Clone() const override;
  operator OnOff*() override { return this; }

  void SetKey(ui::AnsiKey);

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;

  void Parts(const std::function<void(Part&)>& cb) override {
    Object::Parts(cb);
    cb(flip);
  }

  bool IsOn() const override { return current_state; }
  void OnTurnOn() override;
  void OnTurnOff() override;

  std::unique_ptr<ObjectWidget> MakeWidget(ui::Widget* parent) override;
};

}  // namespace automat::library
