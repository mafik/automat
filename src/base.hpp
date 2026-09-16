#pragma once
// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT

#include <include/core/SkCanvas.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkPath.h>
#include <modules/skottie/include/Skottie.h>

#include <algorithm>
#include <cassert>
#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "argument.hpp"
#include "deserializer.hpp"
#include "drag_action.hpp"
#include "error.hpp"
#include "format.hpp"
#include "log.hpp"
#include "pointer.hpp"
#include "prototypes.hpp"
#include "ptr.hpp"
#include "run_button.hpp"
#include "sync.hpp"
#include "tasks.hpp"
#include "widget.hpp"

namespace automat {

using std::deque;
using std::function;
using std::hash;
using std::string;
using std::string_view;
using std::unordered_multimap;
using std::unordered_set;
using std::vector;

struct Error;
struct Object;
struct Location;

// The command that starts an Object's work. While the object's LongRunning is
// active the work is already happening, so this command is inhibited.
struct Runnable : Command {
  struct Table : Command::Table {
    static bool classof(const Interface::Table* i) {
      return Command::Table::classof(i) &&
             static_cast<const Command::Table*>(i)->while_long_running == kInhibit;
    }

    constexpr Table(StrView name) : Command::Table(name, kInhibit) {}
  };

  INTERFACE_BOUND(Runnable, Command)

  template <typename ImplT>
  struct Def : Interface::DefBase {
    using Impl = ImplT;
    using Bound = Runnable;

    static constexpr Table MakeTable() {
      Table t(ImplT::kName);
      t.FillFrom<ImplT>();
      return t;
    }

    inline constinit static Table tbl = MakeTable();

    ~Def() {}
  };
};

using NextArg = InterfaceArgument<Command, Interface::kNextArg>;

struct Scalar : Interface {
  struct Table : Interface::Table {
    static bool classof(const Interface::Table* i) { return i->kind == Interface::kScalar; }

    double (*get)(Scalar) = nullptr;
    void (*set)(Scalar, double) = nullptr;

    constexpr Table(StrView name) : Interface::Table(Interface::kScalar, name) {}

    template <typename ImplT>
    constexpr void FillFrom() {
      Interface::Table::FillFrom<ImplT>();
      get = [](Scalar self) { return static_cast<ImplT&>(self).OnGet(); };
      set = [](Scalar self, double value) { static_cast<ImplT&>(self).OnSet(value); };
    }
  };

  struct State {};

  INTERFACE_BOUND(Scalar, Interface)

  double Get() const { return table->get(*this); }
  void Set(double value) const { table->set(*this, value); }

  template <typename ImplT>
  struct Def : Interface::DefBase {
    using Impl = ImplT;
    using Bound = Scalar;

    static constexpr Table MakeTable() {
      Table t(ImplT::kName);
      t.FillFrom<ImplT>();
      return t;
    }

    inline constinit static Table tbl = MakeTable();

    ~Def() {}
  };
};

struct Text : Interface {
  struct Table : Interface::Table {
    static bool classof(const Interface::Table* i) { return i->kind == Interface::kText; }

    Str (*get)(Text) = nullptr;
    void (*set)(Text, StrView) = nullptr;

    static std::unique_ptr<Action> DefaultActivate(Interface, ui::Pointer&, Toy*);

    constexpr Table(StrView name) : Interface::Table(Interface::kText, name) {
      cursor = ui::Cursor::IBeam;
      activate = &DefaultActivate;
    }

    template <typename ImplT>
    constexpr void FillFrom() {
      Interface::Table::FillFrom<ImplT>();
      get = [](Text self) { return static_cast<ImplT&>(self).OnGet(); };
      set = [](Text self, StrView value) { static_cast<ImplT&>(self).OnSet(value); };
    }
  };

  struct State {};

  INTERFACE_BOUND(Text, Interface)

  Str Get() const { return table->get(*this); }
  void Set(StrView value) const { table->set(*this, value); }

  template <typename ImplT>
  struct Def : Interface::DefBase {
    using Impl = ImplT;
    using Bound = Text;

    static constexpr Table MakeTable() {
      Table t(ImplT::kName);
      t.FillFrom<ImplT>();
      return t;
    }

    inline constinit static Table tbl = MakeTable();

    ~Def() {}
  };
};

// Interface for objects that can hold other objects within.
struct Container {
  // Remove the given `descendant` from this object and return it wrapped in a (possibly newly
  // created) Location.
  virtual Ptr<Location> Extract(Object& descendant) = 0;
};

}  // namespace automat

#include "location.hpp"
