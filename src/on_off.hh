#pragma once
// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include "sync.hh"

namespace automat {

struct OnOff : Syncable {
  struct Table : Syncable::Table {
    static bool classof(const Interface::Table* i) {
      return i->kind >= Interface::kOnOff && i->kind <= Interface::kLastOnOff;
    }

    bool (*is_on)(OnOff) = nullptr;
    void (*on_turn_on)(OnOff) = nullptr;
    void (*on_turn_off)(OnOff) = nullptr;

    static bool DefaultCanSync(Syncable, Syncable other);
    static std::unique_ptr<ui::Widget> DefaultMakeIcon(Argument, ui::Widget* parent);

    constexpr Table(StrView name, Kind kind = Interface::kOnOff) : Syncable::Table(name, kind) {
      can_sync = &DefaultCanSync;
      make_icon = &DefaultMakeIcon;
    }

    template <typename ImplT>
    constexpr void FillFrom() {
      Syncable::Table::FillFrom<ImplT>();
      is_on = [](OnOff self) { return static_cast<const ImplT&>(self).IsOn(); };
      on_turn_on = [](OnOff self) { static_cast<ImplT&>(self).OnTurnOn(); };
      on_turn_off = [](OnOff self) { static_cast<ImplT&>(self).OnTurnOff(); };
    }
  };

  struct State : Syncable::State {};

  INTERFACE_BOUND(OnOff, Syncable)

  bool IsOn() const { return table->is_on(*this); }

  // External entry point: transitions this object AND all synced peers to On.
  // Calls OnTurnOn on self first, then on every synced peer (via ForwardDo).
  // Use when an outside caller (user click, another object) requests the transition.
  void TurnOn() const {
    ForwardDo([](OnOff other) { other.table->on_turn_on(other); });
  }

  // Self-originated notification: this object has already turned On on its own (e.g. detected
  // an OS event, received a hardware signal). Propagates OnTurnOn to all OTHER synced peers,
  // skipping self whose state is already up to date (via ForwardNotify).
  void NotifyTurnedOn() const {
    ForwardNotify([](OnOff other) { other.table->on_turn_on(other); });
  }

  // External entry point: transitions this object AND all synced peers to Off.
  // Calls OnTurnOff on self first, then on every synced peer (via ForwardDo).
  // Use when an outside caller (user click, another object) requests the transition.
  void TurnOff() const {
    ForwardDo([](OnOff other) { other.table->on_turn_off(other); });
  }

  // Self-originated notification: this object has already turned Off on its own (e.g. detected
  // an OS event, received a hardware signal). Propagates OnTurnOff to all OTHER synced peers,
  // skipping self whose state is already up to date (via ForwardNotify).
  void NotifyTurnedOff() const {
    ForwardNotify([](OnOff other) { other.table->on_turn_off(other); });
  }

  void Toggle() const {
    if (IsOn())
      TurnOff();
    else
      TurnOn();
  }

  // ImplT must provide:
  //   using Parent = SomeObject;
  //   static constexpr StrView kName = "..."sv;
  //   static constexpr int Offset();  // offsetof(Parent, def_member)
  //   bool IsOn() const;
  //
  //   // Apply the On/Off state change to THIS object only. Called by the sync infrastructure
  //   // (from ForwardDo or ForwardNotify). Must NOT call TurnOn/TurnOff/NotifyTurnedOn/
  //   // NotifyTurnedOff — doing so would create feedback loops.
  //   void OnTurnOn();
  //   void OnTurnOff();
  template <typename ImplT>
  struct Def : State, DefBase {
    using Impl = ImplT;
    using Bound = OnOff;

    static constexpr Table MakeTable() {
      Table t(ImplT::kName);
      t.FillFrom<ImplT>();
      return t;
    }

    inline constinit static Table tbl = MakeTable();

    ~Def() {
      if (source || !gear_weak.IsExpired()) {
        Bind().Unsync();
      }
    }
  };
};

}  // namespace automat
