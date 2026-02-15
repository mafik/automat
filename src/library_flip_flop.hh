// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "base.hh"
#include "widget.hh"

namespace automat::library {

struct FlipFlop : Object {
  bool current_state = false;

  SyncState flip_sync;
  static Runnable flip;

  SyncState on_off_sync;
  static OnOff on_off;

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
