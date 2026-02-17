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

    Table(StrView name, Kind kind = Interface::kOnOff);
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

    static Table& GetTable() {
      static Table tbl = [] {
        Table t(ImplT::kName);
        t.is_on = +[](OnOff self) -> bool {
          return static_cast<const ImplT&>(self).IsOn();
        };
        t.on_turn_on = +[](OnOff self) {
          static_cast<ImplT&>(self).OnTurnOn();
        };
        t.on_turn_off = +[](OnOff self) {
          static_cast<ImplT&>(self).OnTurnOff();
        };
        t.state_off = ImplT::Offset();
        if constexpr (requires { ImplT::OnSync; }) {
          t.on_sync = +[](Syncable self) {
            static_cast<ImplT&>(self).OnSync();
          };
        }
        if constexpr (requires { ImplT::OnUnsync; }) {
          t.on_unsync = +[](Syncable self) {
            static_cast<ImplT&>(self).OnUnsync();
          };
        }
        return t;
      }();
      return tbl;
    }

    ~Def() {
      if (source || !end.IsExpired()) {
        Bind().Unsync();
      }
    }
  };
};

}  // namespace automat
