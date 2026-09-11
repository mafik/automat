#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include "interface.hpp"
#include "ptr.hpp"

namespace automat {

struct ObjectSource : Interface {
  struct Table : Interface::Table {
    static bool classof(const Interface::Table* i) { return i->kind == Interface::kObjectSource; }

    Ptr<Object> (*take)(ObjectSource) = nullptr;

    constexpr Table(StrView name) : Interface::Table(Interface::kObjectSource, name) {}

    template <typename ImplT>
    constexpr void FillFrom() {
      Interface::Table::FillFrom<ImplT>();
      take = [](ObjectSource self) { return static_cast<ImplT&>(self).OnTake(); };
    }
  };

  struct State {};

  INTERFACE_BOUND(ObjectSource, Interface)

  Ptr<Object> Take() const { return table->take(*this); }

  template <typename ImplT>
  struct Def : Interface::DefBase {
    using Impl = ImplT;
    using Bound = ObjectSource;

    static constexpr Table MakeTable() {
      Table t(ImplT::kName);
      t.FillFrom<ImplT>();
      return t;
    }

    inline constinit static Table tbl = MakeTable();

    ~Def() {}
  };
};

extern ObjectSource::Table kMakeObject;

std::unique_ptr<Action> DragNew(ui::Pointer&, Ptr<Object>&&, std::unique_ptr<Toy>&& toy);

}  // namespace automat
