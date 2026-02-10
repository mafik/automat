#pragma once
// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include <typeinfo>

#include "format.hh"
#include "str.hh"

namespace automat {

// Atom is a base class for most things in Automat.
//
// Although Atom itself doesn't expose much methods (only the ability to identify the specific
// type), it's role is that it's a base for many interfaces.
//
// # Purpose
//
// TL;DR is:
//
// 1. Atoms allow Objects to act in a *generic* way.
// 2. Atoms allow basic code reuse across Objects.
//
// Atoms primary role is to allow objects to behave in a generic fashion. Atoms are like programming
// interfaces that expose different behaviors in a standardized way.
//
// Objects expose their atoms using Object::Atoms function. Automat infrastructure uses this to
// automatically populate menu with various options, help with (de)serialization of state, visualize
// connections between atoms etc.
//
// # Notable subclasses
//
// - Argument (argument.hh/cc) - allows objects to link to (atoms of) other objects
// - Syncable (sync.hh/cc) - allows objects to synchronize their behavior
//
// # Representation
//
// TODO: document:
// 1. memory representation (weak ptr, external atoms)
// 2. menu system / serialization
// 3. visual representation
//
// Atoms are identified by their memory addresses. However, due to concurrent nature of Automat,
// where objects can go away at any time.
//
// # TODOs
//
// TODO: rename & cleanup of Synchronization.md / sync.hh/cc
// TODO: design & document the approach to serialization
// TODO: design & document menu generation
struct Atom {
  virtual ~Atom() = default;

  virtual StrView Name() const {
    const std::type_info& info = typeid(*this);
    return CleanTypeName(info.name());
  }
};

}  // namespace automat
