#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include "interface.hpp"
#include "math.hpp"

namespace automat {

struct Object;

struct Resizable : Interface {
  struct Table : Interface::Table {
    static bool classof(const Interface::Table* i) { return i->kind == Interface::kResizable; }

    bool (*resize_px)(Resizable, int top, int right, int bottom, int left) = nullptr;
    bool (*resize_m)(Resizable, Rect grow) = nullptr;

    constexpr Table(StrView name) : Interface::Table(Interface::kResizable, name) {}
  };

  struct State {};

  INTERFACE_BOUND(Resizable, Interface)

  bool ResizePx(int top, int right, int bottom, int left) const { return table->resize_px ? table->resize_px(*this, top, right, bottom, left) : false; }
  bool ResizeM(Rect grow) const { return table->resize_m ? table->resize_m(*this, grow) : false; }

  // ImplT must provide:
  //   using Parent = SomeObject;
  //   static constexpr StrView kName = "..."sv;
  //   static constexpr int Offset();  // offsetof(Parent, def_member)
  //   bool ResizePx(int top, int right, int bottom, int left);
  //   bool ResizeM(Rect grow);
  template <typename ImplT>
  struct Def : Interface::DefBase {
    using Impl = ImplT;
    using Bound = Resizable;

    template <typename T>
    static bool InvokeResizePx(Resizable self, int top, int right, int bottom, int left) {
      return static_cast<T&>(self).ResizePx(top, right, bottom, left);
    }

    template <typename T>
    static bool InvokeResizeM(Resizable self, Rect grow) {
      return static_cast<T&>(self).ResizeM(grow);
    }

    static constexpr Table MakeTable() {
      Table t(ImplT::kName);
      t.state_off = ImplT::Offset();
      t.resize_px = &InvokeResizePx<ImplT>;
      t.resize_m = &InvokeResizeM<ImplT>;
      return t;
    }

    inline constinit static Table tbl = MakeTable();
  };
};

}  // namespace automat
