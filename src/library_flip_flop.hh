// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "base.hh"
#include "widget.hh"

namespace automat::library {

struct FlipFlop : Object {
  bool current_state = false;

  struct Flip : Runnable {
    void OnRun(std::unique_ptr<RunTask>&) override;

    FlipFlop& GetFlipFlop() const {
      return *reinterpret_cast<FlipFlop*>(reinterpret_cast<intptr_t>(this) -
                                          offsetof(FlipFlop, flip));
    }
  } flip;

  struct State : OnOff {
    bool IsOn() const override { return GetFlipFlop().current_state; }
    void OnTurnOn() override;
    void OnTurnOff() override;

    FlipFlop& GetFlipFlop() const {
      return *reinterpret_cast<FlipFlop*>(reinterpret_cast<intptr_t>(this) -
                                          offsetof(FlipFlop, on_off));
    }
  } on_off;

  FlipFlop();
  ~FlipFlop();
  string_view Name() const override;
  Ptr<Object> Clone() const override;
  operator OnOff*() override { return &on_off; }

  void SetKey(ui::AnsiKey);

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;

  void Parts(const std::function<void(Part&)>& cb) override {
    Object::Parts(cb);
    cb(flip);
    cb(on_off);
  }

  std::unique_ptr<ObjectWidget> MakeWidget(ui::Widget* parent, ReferenceCounted&) override;
};

}  // namespace automat::library
