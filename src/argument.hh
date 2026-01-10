// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <functional>

#include "color.hh"
#include "drawable.hh"
#include "location.hh"
#include "status.hh"

namespace automat {

enum class CableTexture {
  Smooth,
  Braided,
};

// Arguments are responsible for finding dependencies (input & output) of objects.
// - they know about the requirements of the target object (CanConnect)
// - they may automatically create target objects using some prototype (Prototype)
// - they control how the connection is stored (Connect / Disconnect / Find)
// - finally - they control how a connection appears on screen - although this role
//   may be moved into ObjectWidget in the future...
//
// In order to use an Argument, object needs to return it as one of its `Parts()`.
// This however does NOT mean that the Argument must be a member of the object.
// It's possible for Argument to exist outside, and be shared among many objects.
// It just needs to know how to store the connection information. See NextArg as an
// example. (This is a memory micro-optimization - since the goal is to minimize the
// memory footprint of Objects and each polymorphic member adds 8-byte overhead.)
//
// For convenience, Arguments that live within Objects can derive from InlineArgument.
// It provides storage for the connection and default implementations for Connect & Find.
//
// Arguments - like other parts may function as either members of Objects - or as their base class.
// The latter is a good fit if the Object's main purpose overlaps with some relation between
// objects.
//
// Lastly, Arguments provide some helper utilities for lookup of related objects.
// One interesting example is a search for nearby valid objects in a given search radius.
//
// IMPORTANT: Arguments are identified by their ADDRESS in memory (not name!). Don't move them
// around!
//
// Arguments are automatically serialized at the Machine level. It's important that they
// use good `Name()`s. They should be short, human-readable & capitalized.
//
// TODO: think about pointer following
// TODO: think about multiple targets
struct Argument : virtual Part {
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

  virtual void CanConnect(Object& start, Part& end, Status& status) const {
    AppendErrorMessage(status) += "Argument::CanConnect should be overridden";
  }

  bool CanConnect(Object& start, Part& end) const {
    Status status;
    CanConnect(start, end, status);
    return OK(status);
  }

  // This function should register a connection from `start` to the `end` so that subsequent calls
  // to `Find` will return `end`.
  //
  // When `end` is nullptr, this disconnects the existing connection. The implementation can check
  // the current connection value before clearing it (e.g., to call cleanup methods).
  virtual void Connect(Object& start, const NestedPtr<Part>& end) = 0;

  void Disconnect(Object& start) { Connect(start, {}); }

  virtual NestedPtr<Part> Find(Object& start) const = 0;

  // Returns the prototype object for this argument, or nullptr if there is no prototype.
  // This is used by various *OnMake methods to create new object - and for object preview.
  virtual Ptr<Object> Prototype() const { return nullptr; }

  virtual PaintDrawable& Icon();  // TODO: weird - clean this up

  // The returned "to_points" use the target object's local coordinate space.
  void NearbyCandidates(Location& here, float radius,
                        std::function<void(Location&, Vec<Vec2AndDir>& to_points)> callback) const;

  // Use this method if you don't actually care about specific part that the argument points to -
  // just the target object (that owns that part).
  Object* ObjectOrNull(Object& start) const;

  // Use this method if you don't actually care about specific part that the argument points to -
  // just the target object (that owns that part).
  //
  // This is the main way of creating new objects through this Argument's Prototype.
  Object& ObjectOrMake(Object& start) const;
};

struct InlineArgument : Argument {
  NestedWeakPtr<Part> end;

  void Connect(Object&, const NestedPtr<Part>& end) override { this->end = end; }

  NestedPtr<Part> Find(Object&) const override { return end.Lock(); };
};

struct NextArg : Argument {
  StrView Name() const override { return "Next"sv; }
  void CanConnect(Object& start, Part& end, Status&) const override;
  void Connect(Object& start, const NestedPtr<Part>& end) override;
  NestedPtr<Part> Find(Object& start) const override;
};

extern NextArg next_arg;

}  // namespace automat
