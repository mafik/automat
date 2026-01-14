#pragma once
// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include <typeinfo>

#include "format.hh"
#include "str.hh"

namespace automat {

// Part is a virtual(*) base class for most things in Automat.
//
// (*) "virtual" - means that even if some object has a complex, diamond-type inheritance, it's
// always going to have exactly one `Part` base.
//
// Although Part itself doesn't expose much methods (only the ability to identify the specific
// type), it's role is that it's a base for many interfaces.
//
// # Purpose
//
// TL;DR is:
//
// 1. Parts allow Objects to act in a *generic* way.
// 2. Parts allow basic code reuse across Objects.
//
// Parts primary role is to allow objects to behave in a generic fashion. Parts are like programming
// interfaces that expose different behaviors in a standardized way.
//
// Objects expose their parts using Object::Parts function. Automat infrastructure uses this to
// automatically populate menu with various options, help with (de)serialization of state, visualize
// connections between parts etc.
//
// # Notable subclasses
//
// - Argument (argument.hh/cc) - allows objects to link to (parts of) other objects
// - Interface (interface.hh/cc) - allows objects to synchronize their behavior
//
// # Representation
//
// TODO: document:
// 1. memory representation (weak ptr, external parts)
// 2. menu system / serialization
// 3. visual representation
//
// Parts are identified by their memory addresses. However, due to concurrent nature of Automat,
// where objects can go away at any time.
//
// # TODOs
//
// TODO: rename & cleanup of Synchronization.md / interface.hh/cc
// TODO: design & document the approach to serialization
// TODO: design & document menu generation
struct Part {
  virtual ~Part() = default;

  virtual StrView Name() const {
    const std::type_info& info = typeid(*this);
    return CleanTypeName(info.name());
  }
};

}  // namespace automat
