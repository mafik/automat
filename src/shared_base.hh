// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "ptr.hh"

namespace automat {

// Virtual base class for objects that would like to be managed through std::Ptr.
//
// Remember to use virtual inheritance to avoid diamond inheritance issues.
struct SharedBase : public ReferenceCounted {
  virtual ~SharedBase() = default;

  template <class Self>
  WeakPtr<Self> MakeWeakPtr(this const Self& self) {
    // For some reason, `static_pointer_cast` doesn't work with weak_ptr.
    return WeakPtr<Self>(&const_cast<Self&>(self));
  }

  template <class Self>
  Ptr<Self> SharedPtr(this const Self& self) {
    // We have to use dynamic_pointer_cast because `SharedBase` is a virtual base class.
    // Meaning that depending on the Self type, the address of SharedBase may be different.
    // Removing virtual inheritance may allow us to use static_pointer_cast here.
    return DupPtr<Self>(&self);
  }
};

}  // namespace automat