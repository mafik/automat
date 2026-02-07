// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "atom.hh"
#include "ptr.hh"

namespace automat {

// Part is an Atom bound to some memory-managed owner.
//
// Some Atoms within Objects may satisfy the Part concept.
// Objects themselves also satisfy the Part concept - because each Object is also an Atom.
// In fact every Owner (ReferenceCounted) can also function as a "Part", because each
// ReferenceCounted is also an Atom.
template <typename T>
concept Part = requires(T t) {
  { t.GetOwner() } -> std::convertible_to<ReferenceCounted&>;
  { t.GetAtom() } -> std::same_as<Atom&>;
};

}  // namespace automat
