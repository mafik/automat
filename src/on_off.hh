#pragma once
// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include "interfaces.hh"

namespace automat {

struct OnOff : SyncableInterface {
  virtual ~OnOff() { Unsync(); }

  virtual bool IsOn() const = 0;

  void TurnOn() {
    ForwardDo([](OnOff& self) { self.OnTurnOn(); });
  }

  void NotifyTurnedOn() {
    ForwardNotify([](OnOff& self) { self.OnTurnOn(); });
  }

  void TurnOff() {
    ForwardDo([](OnOff& self) { self.OnTurnOff(); });
  }

  void NotifyTurnedOff() {
    ForwardNotify([](OnOff& self) { self.OnTurnOff(); });
  }

  void Toggle() {
    if (IsOn())
      TurnOff();
    else
      TurnOn();
  }

 protected:
  virtual void OnTurnOn() = 0;
  virtual void OnTurnOff() = 0;
};

}  // namespace automat
