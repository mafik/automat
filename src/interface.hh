#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include "str.hh"

namespace automat {

// Interface is the base class for parts of Objects that can be exposed to other Objects.
//
// # Notable subclasses
//
// - Argument (argument.hh/cc) - allows objects to link to (interfaces of) other objects
// - ImageProvider (image_provider.hh) - allows objects to provide image data
//
// # Purpose
//
// 1. Interfaces allow Objects to act in a *generic* way.
// 2. Interfaces allow basic code reuse across Objects.
//
// Objects expose their interfaces using Object::Interfaces function. Automat infrastructure uses
// this to automatically populate menus, help with (de)serialization of state, visualize connections
// between interfaces etc.
//
// Interfaces are identified by their memory addresses. With the static inline pattern, each
// Interface is a class-level static — zero per-instance overhead.
struct Interface {
  enum Kind {
    // Argument and its subclasses (range: kArgument..kLastArgument)
    kArgument,
    kNextArg,      // also an Argument
    kSyncable,     // also an Argument (via Syncable)
    kOnOff,        // also a Syncable
    kLongRunning,  // also an OnOff
    kLastOnOff = kLongRunning,
    kRunnable,  // also a Syncable
    kLastArgument = kRunnable,
    // Standalone interfaces
    kImageProvider,
  };

  Kind kind;
  StrView name;

  constexpr Interface(Kind kind, StrView name) : kind(kind), name(name) {}

  constexpr StrView Name() const { return name; }
};

}  // namespace automat
