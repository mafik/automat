#pragma once
// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT

#include "base.hpp"
#include "widget.hpp"

namespace automat::library {

struct LinkedSwitch : Object {
  Linked<OnOff> on_off;

  DEF_INTERFACE(LinkedSwitch, Runnable, flip, "Flip")
  void OnRun(std::unique_ptr<RunTask>&) {
    if (auto locked = obj->on_off.Lock()) {
      locked.Toggle();
    }
  }
  DEF_END(flip);

  INTERFACES(flip)

  LinkedSwitch(OnOff);
  string_view Name() const override;
  Ptr<Object> Clone() const override;

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;

  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;
};

struct Switch : Object {
  bool current_state = false;

  DEF_INTERFACE(Switch, Runnable, flip, "Flip")
  void OnRun(std::unique_ptr<RunTask>&) { obj->enabled->Toggle(); }
  DEF_END(flip);

  DEF_INTERFACE(Switch, OnOff, enabled, "On/Off")
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

  INTERFACES(flip, enabled);

  Switch() = default;

  string_view Name() const override;
  Ptr<Object> Clone() const override;

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;

  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;
};

}  // namespace automat::library
