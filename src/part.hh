// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "interface.hh"
#include "ptr.hh"

namespace automat {

// Part is an Interface bound to some memory-managed owner.
//
// Some Interfaces within Objects may satisfy the Part concept.
// Objects themselves also satisfy the Part concept - because each Object is also an Interface
// (through Atom). In fact every Owner (ReferenceCounted) can also function as a "Part", because
// each ReferenceCounted is also an Interface (through Atom).
template <typename T>
concept Part = requires(T t) {
  { t.GetOwner() } -> std::convertible_to<ReferenceCounted&>;
  { t.GetInterface() } -> std::same_as<Interface&>;
};

}  // namespace automat
