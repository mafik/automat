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
// - finally - they control how a connection appears on screen - although this role
//   may be moved into Toy in the future...
//
// In order to use an Argument, object needs to return it as one of its `Interfaces()`.
// This however does NOT mean that the Argument must be a member of the object.
// It's possible for Argument to exist outside, and be shared among many objects.
// It just needs to know how to store the connection information. See NextArg as an
// example. (This is a memory micro-optimization - since the goal is to minimize the
// memory footprint of Objects and each polymorphic member adds 8-byte overhead.)
//
// For convenience, Arguments that live within Objects can derive from InlineArgument.
// It provides storage for the connection and default implementations for Connect & Find.
//
// Arguments - like other interfaces may function as either members of Objects - or as their base
// class. The latter is a good fit if the Object's main purpose overlaps with some relation between
// objects.
//
// Lastly, Arguments provide some helper utilities for lookup of related objects.
// One interesting example is a search for nearby valid objects in a given search radius.
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
  enum class Style { Arrow, Cable, Spotlight, Invisible };

  virtual SkColor Tint() const { return "#404040"_color; }
  virtual SkColor Light() const { return "#ef9f37"_color; }
  virtual float AutoconnectRadius() const { return 0_cm; }
  virtual Style GetStyle() const { return Style::Arrow; }

  // Uncomment to find potential bugs related to arguments being moved around.
  // Argument(const Argument&) = delete;
  // Argument& operator=(const Argument&) = delete;
  // Argument(Argument&&) = delete;

  virtual ~Argument() = default;

  virtual void CanConnect(Object& start, Object& end_obj, Interface& end_iface,
                          Status& status) const {
    AppendErrorMessage(status) += "Argument::CanConnect should be overridden";
  }

  bool CanConnect(Object& start, Object& end_obj, Interface& end_iface) const {
    Status status;
    CanConnect(start, end_obj, end_iface, status);
    return OK(status);
  }

  Interface* CanConnect(Object& start, Object& end_obj) const {
    if (CanConnect(start, end_obj, Object::toplevel_interface)) {
      return &Object::toplevel_interface;
    }
    Interface* ret = nullptr;
    end_obj.Interfaces([&](Interface& iface) {
      if (CanConnect(start, end_obj, iface)) {
        ret = &iface;
        return LoopControl::Break;
      }
      return LoopControl::Continue;
    });
    return ret;
  }

  // This function should register a connection from `start` to the `end` so that subsequent calls
  // to `Find` will return `end`.
  //
  // When `end_obj` is nullptr, this disconnects the existing connection. The implementation can
  // check the current connection value before clearing it (e.g., to call cleanup methods).
  virtual void OnConnect(Object& start, Object* end_obj, Interface* end_iface) = 0;

  void Connect(Object& start, Object& end_obj, Interface& end_iface) {
    OnConnect(start, &end_obj, &end_iface);
    start.WakeToys();
  }

  void Connect(Object& start, Object& end_obj) {
    Connect(start, end_obj, Object::toplevel_interface);
  }

  void Disconnect(Object& start) {
    OnConnect(start, nullptr, nullptr);
    start.WakeToys();
  }

  virtual NestedPtr<Interface> Find(const Object& start) const = 0;

  // Returns the prototype object for this argument, or nullptr if there is no prototype.
  // This is used by various *OnMake methods to create new object - and for object preview.
  virtual Ptr<Object> Prototype() const { return nullptr; }

  virtual std::unique_ptr<ui::Widget> MakeIcon(ui::Widget* parent);

  // Use this method if you don't actually care about specific interface that the argument points
  // to - just the target object (that owns that interface).
  Object* ObjectOrNull(Object& start) const;

  // Use this method if you don't actually care about specific interface that the argument points
  // to - just the target object (that owns that interface).
  //
  // This is the main way of creating new objects through this Argument's Prototype.
  Object& ObjectOrMake(Object& start);

  ArgumentOf Of(Object& start);
};

struct ArgumentOf {
  using Toy = ui::ConnectionWidget;
  Object& object;
  Argument& arg;
  ArgumentOf(Object& object, Argument& arg) : object(object), arg(arg) {}

  ReferenceCounted& GetOwner() { return object; }
  Interface& GetInterface() { return arg; }
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent);
};

inline ArgumentOf Argument::Of(Object& start) { return ArgumentOf(start, *this); }

struct InlineArgument : Argument {
  NestedWeakPtr<Interface> end;

  void OnConnect(Object&, Object* end_obj, Interface* end_iface) override {
    if (end_obj) {
      this->end = NestedWeakPtr<Interface>(end_obj->AcquireWeakPtr(), end_iface);
    } else {
      this->end = {};
    }
  }

  NestedPtr<Interface> Find(const Object&) const override { return end.Lock(); };
};

struct NextArg : Argument {
  StrView Name() const override { return "Next"sv; }
  Style GetStyle() const override { return Style::Cable; }
  void CanConnect(Object& start, Object& end_obj, Interface& end_iface, Status&) const override;
  void OnConnect(Object& start, Object* end_obj, Interface* end_iface) override;
  NestedPtr<Interface> Find(const Object& start) const override;
};

extern NextArg next_arg;

}  // namespace automat
