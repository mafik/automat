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
  };

  struct State : Syncable::State {};

  INTERFACE_BOUND(OnOff, Syncable)

  bool IsOn() const { return table->is_on(*this); }

  void TurnOn() const {
    ForwardDo([](OnOff other) { other.table->on_turn_on(other); });
  }

  void NotifyTurnedOn() const {
    ForwardNotify([](OnOff other) { other.table->on_turn_on(other); });
  }

  void TurnOff() const {
    ForwardDo([](OnOff other) { other.table->on_turn_off(other); });
  }

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
  //   static bool IsOn(const Parent&);
  //   static void OnTurnOn(Parent&);
  //   static void OnTurnOff(Parent&);
  template <typename ImplT>
  struct Def : State, DefBase {
    using Impl = ImplT;
    using Bound = OnOff;

    template <typename T>
    static bool InvokeIsOn(OnOff self) {
      return static_cast<const T&>(self).IsOn();
    }
    template <typename T>
    static void InvokeOnTurnOn(OnOff self) {
      static_cast<T&>(self).OnTurnOn();
    }
    template <typename T>
    static void InvokeOnTurnOff(OnOff self) {
      static_cast<T&>(self).OnTurnOff();
    }
    template <typename T>
    static void InvokeOnSync(Syncable self) {
      static_cast<T&>(self).OnSync();
    }
    template <typename T>
    static void InvokeOnUnsync(Syncable self) {
      static_cast<T&>(self).OnUnsync();
    }

    static constexpr Table MakeTable() {
      Table t(ImplT::kName);
      t.state_off = ImplT::Offset();
      t.is_on = &InvokeIsOn<ImplT>;
      t.on_turn_on = &InvokeOnTurnOn<ImplT>;
      t.on_turn_off = &InvokeOnTurnOff<ImplT>;
      if constexpr (requires { ImplT::OnSync; })
        t.on_sync = &InvokeOnSync<ImplT>;
      if constexpr (requires { ImplT::OnUnsync; })
        t.on_unsync = &InvokeOnUnsync<ImplT>;
      if constexpr (requires { ImplT::kTint; })
        t.tint = ImplT::kTint;
      return t;
    }

    inline constinit static Table tbl = MakeTable();

    ~Def() {
      if (source || !end.IsExpired()) {
        Bind().Unsync();
      }
    }
  };
};

}  // namespace automat
