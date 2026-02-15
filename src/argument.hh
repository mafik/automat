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

struct ArgumentOf;

// Arguments are responsible for finding dependencies (input & output) of objects.
// - they know about the requirements of the target object (CanConnect)
// - they may automatically create target objects using some prototype (Prototype)
// - they control how the connection is stored (Connect / Disconnect / Find)
// - finally - they control how a connection appears on screen
//
// In order to use an Argument, object needs to return it as one of its `Interfaces()`.
//
// With the static inline pattern, Arguments are class-level statics shared by all instances
// of an Object type. Function pointers replace virtual methods, and per-instance connection
// state is stored in the Object (via SyncState or direct members).
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
  static bool classof(const Interface* i) {
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

  // Checks whether it's possible to connect this argument to the given `end_obj` Object +
  // `end_iface` Interface. `end_iface` may be nullptr if the connection should terminate at the
  // top-level `end_obj` rather than one of its interfaces.
  //
  // Issues should be reported through the Status argument.
  void (*can_connect)(const Argument&, Object& start, Object& end_obj, Interface* end_iface,
                      Status&) = nullptr;

  // Establishes connection between the `start` object and the `end_obj` + `end_iface` destination.
  //
  // `end_obj` may be nullptr if the connection should be disconnected.
  //
  // `end_iface` may be nullptr if the connection should terminate at the top-level `end_obj` rather
  // than one of its interfaces.
  //
  // If non-null, `end_obj` is guaranteed to be alive during this call, but it's not guaranteed
  // exist afterwards. In order to safely store the reference to `end_obj`, it should rely on
  // WeakPtr (or NestedWeakPtr).
  void (*on_connect)(const Argument&, Object& start, Object* end_obj,
                     Interface* end_iface) = nullptr;

  // Looks up the destination of this Argument. This sholud match the last `on_connect`.
  NestedPtr<Interface> (*find)(const Argument&, const Object& start) =
      [](const Argument&, const Object& start) { return NestedPtr<Interface>(); };

  // Prototype for automatically constructed value for this argument. It is used if this Argument is
  // used through one of the *OrMake functions.
  Ptr<Object> (*prototype)() = []() { return Ptr<Object>(nullptr); };

  // Creates a small icon used as a symbol (or label) for this connection. Should be ~1x1cm.
  std::unique_ptr<ui::Widget> (*make_icon)(const Argument&, ui::Widget* parent) = nullptr;

  Argument(StrView name, Kind kind = Interface::kArgument) : Interface(kind, name) {}

  // Convenience wrappers that delegate to function pointers

  void CanConnect(Object& start, Object& end_obj, Interface* end_iface, Status& status) const {
    if (can_connect)
      can_connect(*this, start, end_obj, end_iface, status);
    else
      AppendErrorMessage(status) += Str(Name()) + "::CanConnect not implemented";
  }

  bool CanConnect(Object& start, Object& end_obj, Interface* end_iface) const {
    Status status;
    CanConnect(start, end_obj, end_iface, status);
    return OK(status);
  }

  // Try connecting to the top-level object first, then each sub-interface.
  // Returns the first compatible interface (or nullptr for top-level), or nullopt if none.
  std::optional<Interface*> CanConnect(Object& start, Object& end_obj) const {
    if (CanConnect(start, end_obj, nullptr)) {
      return nullptr;
    }
    std::optional<Interface*> ret;
    end_obj.Interfaces([&](Interface& iface) {
      if (CanConnect(start, end_obj, &iface)) {
        ret = &iface;
        return LoopControl::Break;
      }
      return LoopControl::Continue;
    });
    return ret;
  }

  void OnConnect(Object& start, Object* end_obj, Interface* end_iface) const {
    if (on_connect) on_connect(*this, start, end_obj, end_iface);
  }

  void Connect(Object& start, Object& end_obj, Interface& end_iface) const {
    OnConnect(start, &end_obj, &end_iface);
    start.WakeToys();
  }

  void Connect(Object& start, Object& end_obj) const {
    OnConnect(start, &end_obj, nullptr);
    start.WakeToys();
  }

  void Disconnect(Object& start) const {
    OnConnect(start, nullptr, nullptr);
    start.WakeToys();
  }

  NestedPtr<Interface> Find(const Object& start) const { return find(*this, start); }

  std::unique_ptr<ui::Widget> MakeIcon(ui::Widget* parent) const;

  // Use this method if you don't actually care about specific interface that the argument points
  // to - just the target object (that owns that interface).
  Object* ObjectOrNull(Object& start) const;

  // Use this method if you don't actually care about specific interface that the argument points
  // to - just the target object (that owns that interface).
  //
  // This is the main way of creating new objects through this Argument's Prototype.
  Object& ObjectOrMake(Object& start) const;

  ArgumentOf Of(Object& start);
};

struct ArgumentOf {
  using Toy = ui::ConnectionWidget;
  Object& object;
  Argument& arg;
  ArgumentOf(Object& object, Argument& arg) : object(object), arg(arg) {}

  ReferenceCounted& GetOwner() { return object; }
  Interface* GetInterface() { return &arg; }
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent);
};

inline ArgumentOf Argument::Of(Object& start) { return ArgumentOf(start, *this); }

// Per-instance state for a NextArg interface.
// Each Object that exposes a NextArg stores one NextState.
struct NextState {
  NestedWeakPtr<Runnable> next;
};

// An Argument subclass for "Next" connections — triggers execution of a linked Runnable.
//
// Each object type that wants a "Next" connection defines its own `static NextArg`.
// This replaces the old global `next_arg` singleton + `SignalNext` mixin.
struct NextArg : Argument {
  static bool classof(const Interface* i) { return i->kind == Interface::kNextArg; }

  NextState& (*get_next_state)(Object&) = nullptr;

  NextArg(StrView name);

  template <typename T>
  NextArg(StrView name, NextState& (*get)(T&)) : NextArg(name) {
    get_next_state = reinterpret_cast<NextState& (*)(Object&)>(get);
  }
};

}  // namespace automat
