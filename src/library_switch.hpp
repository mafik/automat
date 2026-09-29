#pragma once
// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT

#include "base.hpp"
#include "widget.hpp"

namespace automat::library {

struct LinkedSwitch : Object {
  DEF_INTERFACE(LinkedSwitch, InterfaceArgument<OnOff>, on_off, "On/Off")
  static constexpr auto kStyle = Argument::Style::RoutedCable;
  static constexpr bool kVisibleWhenDisconnected = false;
  static constexpr SkColor4f kTint = "#216778"_color4f;
  DEF_END(on_off);

  DEF_INTERFACE(LinkedSwitch, Command, flip, "Flip")
  void OnRun() {
    if (auto locked = obj->on_off->FindInterface()) {
      locked.Toggle();
      obj->on_off->LastActivity()->fetch_add(1, std::memory_order_relaxed);
    }
  }
  DEF_END(flip);

  INTERFACES(flip_tbl, on_off_tbl);

  LinkedSwitch(OnOff);
  string_view Name() const override;
  Ptr<Object> Clone() const override;

  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;
};

struct Switch : Object {
  bool current_state = false;

  DEF_INTERFACE(Switch, Command, flip, "Flip")
  void OnRun() { obj->enabled->Toggle(); }
  DEF_END(flip);

  DEF_INTERFACE(Switch, OnOff, enabled, "On/Off")
  bool IsOn() const { return obj->current_state; }
  static constexpr int TurnOnIdx() { return SlotOf(switch_on_tbl); }
  static constexpr int TurnOffIdx() { return SlotOf(switch_off_tbl); }
  DEF_END(enabled);

  DEF_INTERFACE(Switch, Command, switch_on, "Switch on")
  void OnRun() {
    obj->current_state = true;
    obj->WakeToys();
  }
  DEF_END(switch_on);

  DEF_INTERFACE(Switch, Command, switch_off, "Switch off")
  void OnRun() {
    obj->current_state = false;
    obj->WakeToys();
  }
  DEF_END(switch_off);

  INTERFACES(flip_tbl, enabled_tbl, switch_on_tbl, switch_off_tbl);

  Switch() = default;

  string_view Name() const override;
  Ptr<Object> Clone() const override;

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;

  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;
};

}  // namespace automat::library
