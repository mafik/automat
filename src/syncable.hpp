#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "interface.hpp"

namespace automat {

struct Syncable : Interface {
  struct Table : Interface::Table {
    static bool classof(const Interface::Table* i) { return i->kind == Interface::kSyncable; }

    void (*on_sync)(Syncable) = nullptr;
    void (*on_unsync)(Syncable) = nullptr;

    constexpr Table(StrView name) : Interface::Table(Interface::kSyncable, name) {}

    template <typename ImplT>
    constexpr void FillFrom() {
      Interface::Table::FillFrom<ImplT>();
      if constexpr (requires(ImplT& i) { i.OnSync(); })
        on_sync = [](Syncable self) { static_cast<ImplT&>(self).OnSync(); };
      if constexpr (requires(ImplT& i) { i.OnUnsync(); })
        on_unsync = [](Syncable self) { static_cast<ImplT&>(self).OnUnsync(); };
    }
  };

  struct State {};

  INTERFACE_BOUND(Syncable, Interface)

  template <typename ImplT>
  struct Def : Interface::DefBase {
    using Impl = ImplT;
    using Bound = Syncable;

    static constexpr Table MakeTable() {
      Table t(ImplT::kName);
      t.FillFrom<ImplT>();
      return t;
    }

    inline constinit static Table tbl = MakeTable();
  };
};

}  // namespace automat
