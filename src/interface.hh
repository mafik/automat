#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include <typeinfo>

#include "format.hh"
#include "str.hh"

namespace automat {

// Interface is the base class for parts of Objects that can be exposed to other Objects.
//
// # Notable subclasses
//
// - Argument (argument.hh/cc) - allows objects to link to (interfaces of) other objects
// - ImageProvider (image_provider.hh) - allows objects to provide image data
//
// This means that Objects and Locations themselves are Interfaces too (through Atom →
// ReferenceCounted / Trackable).
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
// Interfaces are identified by their memory addresses.
struct Interface {
  virtual ~Interface() = default;

  virtual StrView Name() const {
    const std::type_info& info = typeid(*this);
    return CleanTypeName(info.name());
  }
};

}  // namespace automat
