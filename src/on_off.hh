#pragma once
// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include "sync.hh"

namespace automat {

struct OnOff : Syncable {
  static bool classof(const Interface* i) {
    return i->kind >= Interface::kOnOff && i->kind <= Interface::kLastOnOff;
  }

  // Function pointers for OnOff behavior.
  bool (*is_on)(const OnOff&, const Object&) = nullptr;
  void (*on_turn_on)(const OnOff&, Object&) = nullptr;
  void (*on_turn_off)(const OnOff&, Object&) = nullptr;

  OnOff(StrView name, Kind kind = Interface::kOnOff);

  template <typename T>
  OnOff(StrView name, SyncState& (*get)(T&),
        bool (*is_on_fn)(const OnOff&, const T&), void (*on_turn_on_fn)(const OnOff&, T&),
        void (*on_turn_off_fn)(const OnOff&, T&), Kind kind = Interface::kOnOff)
      : OnOff(name, kind) {
    get_sync_state = reinterpret_cast<SyncState& (*)(Object&)>(get);
    is_on = reinterpret_cast<bool (*)(const OnOff&, const Object&)>(is_on_fn);
    on_turn_on = reinterpret_cast<void (*)(const OnOff&, Object&)>(on_turn_on_fn);
    on_turn_off = reinterpret_cast<void (*)(const OnOff&, Object&)>(on_turn_off_fn);
  }

  bool IsOn(const Object& self) const {
    return is_on ? is_on(*this, self) : false;
  }

  void TurnOn(Object& self) const {
    ForwardDo<OnOff>(self, [](Object& o, const OnOff& iface) {
      if (iface.on_turn_on) iface.on_turn_on(iface, o);
    });
  }

  void NotifyTurnedOn(Object& self) const {
    ForwardNotify<OnOff>(self, [](Object& o, const OnOff& iface) {
      if (iface.on_turn_on) iface.on_turn_on(iface, o);
    });
  }

  void TurnOff(Object& self) const {
    ForwardDo<OnOff>(self, [](Object& o, const OnOff& iface) {
      if (iface.on_turn_off) iface.on_turn_off(iface, o);
    });
  }

  void NotifyTurnedOff(Object& self) const {
    ForwardNotify<OnOff>(self, [](Object& o, const OnOff& iface) {
      if (iface.on_turn_off) iface.on_turn_off(iface, o);
    });
  }

  void Toggle(Object& self) const {
    if (IsOn(self))
      TurnOff(self);
    else
      TurnOn(self);
  }
};

}  // namespace automat
