// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "color.hh"
#include "control_flow.hh"
#include "object.hh"
#include "status.hh"
#include "units.hh"

namespace automat {

namespace ui {
struct Widget;
struct ConnectionWidget;
}  // namespace ui

enum class CableTexture {
  Smooth,
  Braided,
};

// Arguments are responsible for finding dependencies (input & output) of objects.
// - they know about the requirements of the target object (CanConnect)
// - they may automatically create target objects using some prototype (Prototype)
// - they control how the connection is stored (Connect / Disconnect / Find)
// - finally - they control how a connection appears on screen
//
// In order to use an Argument, object needs to return it as one of its `Interfaces()`.
//
// With the Def<ImplT> pattern, each Argument's static Table is auto-generated per ImplT.
// Function pointers replace virtual methods, and per-instance state lives in Def members.
//
// IMPORTANT: Arguments are identified by their ADDRESS in memory (not name!). Don't move them
// around!
//
// Arguments are automatically serialized at the Board level. It's important that they
// use good `Name()`s. They should be short, human-readable & capitalized.
//
// TODO: think about pointer following
// TODO: think about multiple targets
struct Argument : Interface {
  struct Table : Interface::Table {
    static bool classof(const Interface::Table* i) {
      return i->kind >= Interface::kArgument && i->kind <= Interface::kLastArgument;
    }

    enum class Style { Arrow, Cable, Spotlight, Invisible };

    // The primary color of this connection.
    SkColor tint = "#404040"_color;

    // The color of the "blink" light of the cable.
    SkColor light = "#ef9f37"_color;

    // How far the connection will look around to auto-snap to compatible objects.
    float autoconnect_radius = 0_cm;

    // How the connection should be rendered.
    Style style = Style::Arrow;

    // Checks whether it's possible to connect this argument to the given end.
    // end.table may be nullptr if the connection targets the object itself (not a specific
    // interface). Issues should be reported through the Status argument.
    void (*can_connect)(Argument, Interface end, Status&) = nullptr;

    // Establishes or breaks a connection. A null end means disconnect.
    // If end.object_ptr is non-null, it's guaranteed to be alive during this call but not afterwards.
    // Use WeakPtr/NestedWeakPtr to store the reference.
    void (*on_connect)(Argument, Interface end) = nullptr;

    // Looks up the destination of this Argument. This should match the last `on_connect`.
    NestedPtr<Interface::Table> (*find)(Argument) =
        [](Argument) { return NestedPtr<Interface::Table>(); };

    // Prototype for automatically constructed value for this argument. It is used if this Argument
    // is used through one of the *OrMake functions.
    Ptr<Object> (*prototype)() = []() { return Ptr<Object>(nullptr); };

    // Creates a small icon used as a symbol (or label) for this connection. Should be ~1x1cm.
    std::unique_ptr<ui::Widget> (*make_icon)(Argument, ui::Widget* parent) = nullptr;

    Table(StrView name, Kind kind = Interface::kArgument) : Interface::Table(kind, name) {}
  };

  // Note: the `Toy` alias is inherited by Syncable, OnOff, Runnable, LongRunning and leaks
  // into their nested Table scopes. Code inside Table methods should use `automat::Toy` if
  // they need the base Toy type.
  using Toy = ui::ConnectionWidget;
  using Style = Table::Style;

  struct State {};
  INTERFACE_BOUND(Argument, Interface)
  Argument(Object& obj) : Interface(obj) {}

  ReferenceCounted& GetOwner() { return *object_ptr; }
  Interface::Table* GetInterface() { return table_ptr; }
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent);

  // --- Public API (bound type methods) ---

  void CanConnect(Interface end, Status& status) const {
    if (table->can_connect)
      table->can_connect(*this, end, status);
    else
      AppendErrorMessage(status) += Str(Name()) + "::CanConnect not implemented";
  }

  bool CanConnect(Interface end) const {
    Status status;
    CanConnect(end, status);
    return OK(status);
  }

  // Try connecting to the top-level object first, then each sub-interface.
  // Returns the first compatible interface (or nullptr for top-level), or nullopt if none.
  std::optional<Interface::Table*> CanConnect(Object& end_obj) const {
    if (CanConnect(Interface(end_obj))) {
      return nullptr;
    }
    std::optional<Interface::Table*> ret;
    end_obj.Interfaces([&](Interface::Table& iface) {
      if (CanConnect(Interface(end_obj, iface))) {
        ret = &iface;
        return LoopControl::Break;
      }
      return LoopControl::Continue;
    });
    return ret;
  }

  void Connect(Interface end) const {
    if (table->on_connect) table->on_connect(*this, end);
    object_ptr->WakeToys();
  }

  void Disconnect() const {
    Connect(Interface());
  }

  NestedPtr<Interface::Table> Find() const { return table->find(*this); }

  std::unique_ptr<ui::Widget> MakeIcon(ui::Widget* parent) const;

  Object* ObjectOrNull() const;

  Object& ObjectOrMake() const;

  // v2-style definition: stateless, static Table auto-generated from ImplT.
  //
  // ImplT must provide:
  //   using Parent = SomeObject;
  //   static constexpr StrView kName = "..."sv;
  //   static constexpr int Offset();  // offsetof(Parent, def_member)
  //   static void Configure(Table&);  // populate function pointers & style fields
  template <typename ImplT>
  struct Def : Interface::DefBase {
    using Impl = ImplT;
    using Bound = Argument;

    static Table& GetTable() {
      static Table tbl = [] {
        Table t(ImplT::kName);
        t.state_off = ImplT::Offset();
        ImplT::Configure(t);
        return t;
      }();
      return tbl;
    }
  };
};

// An Argument subclass for "Next" connections — triggers execution of a linked Runnable.
struct NextArg : Argument {
  struct State {
    // Points to a Runnable::Table at runtime. Uses Interface::Table to avoid circular includes.
    NestedWeakPtr<Interface::Table> next;
  };

  struct Table : Argument::Table {
    static bool classof(const Interface::Table* i) { return i->kind == Interface::kNextArg; }

    Table(StrView name);
  };

  INTERFACE_BOUND(NextArg, Argument)

  // v2-style definition: per-instance State, static Table auto-generated from ImplT.
  //
  // ImplT must provide:
  //   using Parent = SomeObject;
  //   static constexpr StrView kName = "..."sv;
  //   static constexpr int Offset();  // offsetof(Parent, def_member)
  template <typename ImplT>
  struct Def : State, Interface::DefBase {
    using Impl = ImplT;
    using Bound = NextArg;

    static Table& GetTable() {
      static Table tbl = [] {
        Table t(ImplT::kName);
        t.state_off = ImplT::Offset();
        if constexpr (requires { ImplT::Configure; }) {
          ImplT::Configure(t);
        }
        return t;
      }();
      return tbl;
    }
  };
};


}  // namespace automat
