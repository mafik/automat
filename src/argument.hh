// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "casting.hh"
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

struct ArgumentToy : Toy {
  using Toy::Toy;
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
    // If end.object_ptr is non-null, it's guaranteed to be alive during this call but not
    // afterwards. Use WeakPtr/NestedWeakPtr to store the reference.
    void (*on_connect)(Argument, Interface end) = nullptr;

    // Looks up the destination of this Argument. This should match the last `on_connect`.
    NestedPtr<Interface::Table> (*find)(Argument) = [](Argument) {
      return NestedPtr<Interface::Table>();
    };

    // Prototype for automatically constructed value for this argument. It is used if this Argument
    // is used through one of the *OrMake functions.
    Ptr<Object> (*prototype)() = []() { return Ptr<Object>(nullptr); };

    static std::unique_ptr<ui::Widget> DefaultMakeIcon(Argument, ui::Widget* parent);

    // Creates a small icon used as a symbol (or label) for this connection. Should be ~1x1cm.
    std::unique_ptr<ui::Widget> (*make_icon)(Argument, ui::Widget* parent) = DefaultMakeIcon;

    constexpr Table(StrView name, Kind kind = Interface::kArgument)
        : Interface::Table(kind, name) {}

    template <typename ImplT>
    constexpr void FillFrom() {
      Interface::Table::FillFrom<ImplT>();
      if constexpr (requires { ImplT::kStyle; }) style = ImplT::kStyle;
      if constexpr (requires { ImplT::kAutoconnectRadius; })
        autoconnect_radius = ImplT::kAutoconnectRadius;
      if constexpr (requires { ImplT::kTint; }) tint = ImplT::kTint;
      if constexpr (requires(ImplT& i, Interface e, Status& s) { i.OnCanConnect(e, s); })
        can_connect = [](Argument self, Interface end, Status& s) {
          static_cast<ImplT&>(self).OnCanConnect(end, s);
        };
      if constexpr (requires(ImplT& i, Interface e) { i.OnConnect(e); })
        on_connect = [](Argument self, Interface end) { static_cast<ImplT&>(self).OnConnect(end); };
      if constexpr (requires(ImplT& i) {
                      { i.OnFind() } -> std::same_as<NestedPtr<Interface::Table>>;
                    })
        find = [](Argument self) { return static_cast<ImplT&>(self).OnFind(); };
      if constexpr (requires(ImplT& i, ui::Widget* p) { i.OnMakeIcon(p); })
        make_icon = [](Argument self, ui::Widget* p) {
          return static_cast<ImplT&>(self).OnMakeIcon(p);
        };
      if constexpr (requires { &ImplT::MakePrototype; })
        prototype = []() { return ImplT::MakePrototype(); };
    }
  };

  // Note: the `Toy` alias is inherited by derived interface types and leaks
  // into their nested Table scopes. Code inside Table methods should use `automat::Toy` if
  // they need the base Toy type.
  using Toy = ArgumentToy;
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
    end_obj.Interfaces([&](Interface iface) {
      if (CanConnect(iface)) {
        ret = iface.table_ptr;
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

  void Disconnect() const { Connect(Interface()); }

  NestedPtr<Interface::Table> Find() const { return table->find(*this); }

  std::unique_ptr<ui::Widget> MakeIcon(ui::Widget* parent) const;

  Object* ObjectOrNull() const;

  Object& ObjectOrMake() const;

  // ImplT may optionally provide:
  //   static constexpr Style kStyle = ...;
  //   static constexpr float kAutoconnectRadius = ...;
  //   static constexpr SkColor kTint = ...;
  //   void OnCanConnect(Interface end, Status& status);
  //   void OnConnect(Interface end);
  //   NestedPtr<Interface::Table> OnFind();
  //   std::unique_ptr<ui::Widget> OnMakeIcon(ui::Widget* parent);
  //   static Ptr<Object> MakePrototype();
  template <typename ImplT>
  struct Def : Interface::DefBase {
    using Impl = ImplT;
    using Bound = Argument;

    static constexpr Table MakeTable() {
      Table t(ImplT::kName);
      t.FillFrom<ImplT>();
      return t;
    }

    inline constinit static Table tbl = MakeTable();
  };
};

// ObjectArgument<T> — connects to a top-level object of type T.
// Stores WeakPtr<T> as State::target. Provides defaults for CanConnect/OnConnect/Find.
template <typename T>
struct ObjectArgument : Argument {
  struct State : Argument::State {
    WeakPtr<T> target;
  };

  INTERFACE_BOUND(ObjectArgument, Argument)

  struct Table : Argument::Table {
    static bool classof(const Interface::Table* i) { return i->kind == Interface::kObjectArgument; }

    static void DefaultCanConnect(Argument, Interface end, Status& status) {
      if (end.has_table() || !dynamic_cast<T*>(end.object_ptr)) {
        AppendErrorMessage(status) += "Wrong connection type";
      }
    }

    static void DefaultOnConnect(Argument self, Interface end) {
      State& st = *cast<ObjectArgument>(self).state;
      if (auto* target = dynamic_cast<T*>(end.object_ptr)) {
        st.target = target->AcquireWeakPtr();
      } else {
        st.target = {};
      }
    }

    static NestedPtr<Interface::Table> DefaultFind(Argument self) {
      State& st = *cast<ObjectArgument>(self).state;
      return NestedPtr<Interface::Table>(st.target.Lock(), nullptr);
    }

    constexpr Table(StrView name) : Argument::Table(name, Interface::kObjectArgument) {
      can_connect = &DefaultCanConnect;
      on_connect = &DefaultOnConnect;
      find = &DefaultFind;
    }
  };

  template <typename ImplT>
  struct Def : State, Interface::DefBase {
    using Impl = ImplT;
    using Bound = ObjectArgument<T>;

    static constexpr Table MakeTable() {
      Table t(ImplT::kName);
      t.template FillFrom<ImplT>();
      return t;
    }

    inline constinit static Table tbl = MakeTable();
  };

  Ptr<T> FindObject() const {
    auto nested_ptr = table->find(*this);
    return nested_ptr.template Owner<T>()->AcquirePtr();
  }

  WeakPtr<T> FindObjectWeak() const { return state->target; }

  using Toy = ui::ConnectionWidget;
};

// InterfaceArgument<T, kKind> — connects to a specific interface of type T.
// Stores NestedWeakPtr<T::Table> as State::target. Provides defaults for
// CanConnect/OnConnect/Find. Set kKind = kNextArg to get the NextArg specialization.
template <typename T, Interface::Kind kKind = Interface::kInterfaceArgument>
struct InterfaceArgument : Argument {
  struct State : Argument::State {
    NestedWeakPtr<typename T::Table> target;
  };

  INTERFACE_BOUND(InterfaceArgument, Argument)

  struct Table : Argument::Table {
    static bool classof(const Interface::Table* i) {
      if constexpr (kKind == Interface::kInterfaceArgument)
        return i->kind >= Interface::kInterfaceArgument &&
               i->kind <= Interface::kLastInterfaceArgument;
      else
        return i->kind == kKind;
    }

    static void DefaultCanConnect(Argument, Interface end, Status& status) {
      if (!dyn_cast_if_present<typename T::Table>(end.table_ptr)) {
        AppendErrorMessage(status) += "Wrong interface type";
      }
    }

    static void DefaultOnConnect(Argument self, Interface end) {
      cast<InterfaceArgument>(self).state->target = dyn_cast_if_present<T>(end);
    }

    static NestedPtr<Interface::Table> DefaultFind(Argument self) {
      return cast<InterfaceArgument>(self).state->target.Lock();
    }

    constexpr Table(StrView name) : Argument::Table(name, kKind) {
      style = Style::Cable;
      can_connect = &DefaultCanConnect;
      on_connect = &DefaultOnConnect;
      find = &DefaultFind;
      if constexpr (kKind == Interface::kNextArg) make_icon = &DefaultMakeIcon;
    }
  };

  NestedPtr<typename T::Table> FindInterface() const {
    return Find().template Cast<typename T::Table>();
  }

  template <typename ImplT>
  struct Def : State, Interface::DefBase {
    using Impl = ImplT;
    using Bound = InterfaceArgument<T, kKind>;

    static constexpr Table MakeTable() {
      Table t(ImplT::kName);
      t.template FillFrom<ImplT>();
      return t;
    }

    inline constinit static Table tbl = MakeTable();
  };
};

}  // namespace automat
