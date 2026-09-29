#pragma once
// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include "basic_command.hpp"
#include "menu.hpp"
#include "pointer.hpp"
#include "sync.hpp"

namespace automat {

struct OnOff : Syncable {
  struct Table : Syncable::Table {
    static bool classof(const Interface::Table* i) {
      return i->kind >= Interface::kOnOff && i->kind <= Interface::kLastOnOff;
    }

    bool (*is_on)(OnOff) = nullptr;
    void (*on_turn_on)(OnOff) = nullptr;
    void (*on_turn_off)(OnOff) = nullptr;

    int turn_on_idx = 0;
    int turn_off_idx = 0;

    static bool DefaultCanSync(Syncable, Syncable other);

    constexpr Table(StrView name, Kind kind = Interface::kOnOff) : Syncable::Table(name, kind) {
      can_sync = &DefaultCanSync;
      fill_menu = [](Interface self, Menu& menu) {
        Syncable::Table::DefaultFillMenu(self, menu);
        auto on_off = cast<OnOff>(self);
        int idx = on_off.IsOn() ? on_off.table->turn_off_idx : on_off.table->turn_on_idx;
        menu.Place(ui::Dir::N, self.object_ptr->InterfaceFromIdx(idx));
      };
      on_idle = [](Syncable self) {
        auto on_off = cast<OnOff>(self);
        if (on_off.IsOn()) on_off.table->on_turn_off(on_off);
      };
      activate = [](Interface self, ui::Pointer& pointer,
                    automat::Toy*) -> std::unique_ptr<Action> {
        cast<OnOff>(self).Toggle();
        return std::make_unique<EmptyAction>(pointer);
      };
      cursor = ui::Cursor::Hand;
    }

    template <typename ImplT>
    constexpr void FillFrom() {
      Syncable::Table::FillFrom<ImplT>();
      is_on = [](OnOff self) { return static_cast<const ImplT&>(self).IsOn(); };
      turn_on_idx = ImplT::TurnOnIdx();
      turn_off_idx = ImplT::TurnOffIdx();
      on_turn_on = [](OnOff self) {
        auto turn_on = self.object_ptr->InterfaceFromIdx<BasicCommand>(self.table->turn_on_idx);
        turn_on.table->on_run(turn_on);
      };
      on_turn_off = [](OnOff self) {
        auto turn_off = self.object_ptr->InterfaceFromIdx<BasicCommand>(self.table->turn_off_idx);
        turn_off.table->on_run(turn_off);
      };
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
      if (IsSource() || HasGear()) {
        Bind().Unsync();
      }
    }
  };
};

}  // namespace automat
