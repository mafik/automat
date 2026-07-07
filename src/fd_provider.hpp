#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "interface.hpp"
#include "status.hpp"
#include "str.hpp"

namespace automat {

struct Object;

// Interface for objects that stand for an openable file. A consumer wiring
// its descriptors at start (a Command installing stdio) resolves the object
// to a concrete file descriptor and owns the result: it installs the
// descriptor and closes its copy. Resolution is a fresh open on every call,
// so a rerun rereads or rewrites the file the way a shell redirection does;
// sharing one open description (one file offset) would make reruns resume
// where the last run stopped.
struct FdProvider : Interface {
  enum class Dir : uint8_t { Read, Write };

  struct Table : Interface::Table {
    static bool classof(const Interface::Table* i) { return i->kind == Interface::kFdProvider; }

    // Returns an owned descriptor open for `dir`, or -1 with `status`
    // filled. Called at consumer start, off the UI thread.
    int (*resolve)(FdProvider, Dir, Status&) = nullptr;

    constexpr Table(StrView name) : Interface::Table(Interface::kFdProvider, name) {}
  };

  struct State {};

  INTERFACE_BOUND(FdProvider, Interface)

  int Resolve(Dir dir, Status& status) const {
    if (!table->resolve) {
      AppendErrorMessage(status) += "not openable";
      return -1;
    }
    return table->resolve(*this, dir, status);
  }

  // ImplT must provide:
  //   static constexpr StrView kName = "..."sv;
  //   static constexpr int Offset();  // offsetof(Parent, def_member)
  //   int OnResolve(Dir, Status&);
  template <typename ImplT>
  struct Def : Interface::DefBase {
    using Impl = ImplT;
    using Bound = FdProvider;

    static constexpr Table MakeTable() {
      Table t(ImplT::kName);
      t.state_off = ImplT::Offset();
      t.resolve = [](FdProvider self, Dir dir, Status& status) {
        return static_cast<ImplT&>(self).OnResolve(dir, status);
      };
      return t;
    }

    inline constinit static Table tbl = MakeTable();
  };
};

// The FdProvider among `obj`'s interfaces, or a null-bound one. Stream
// connections target stream ports; the descriptor contract lives beside the
// port on the same object, and consumers look it up through this.
FdProvider FindFdProvider(Object& obj);

}  // namespace automat
