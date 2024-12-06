// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <memory>

namespace automat {

// Virtual base class for objects that would like to be managed through std::shared_ptr.
//
// Remember to use virtual inheritance to avoid diamond inheritance issues.
struct SharedBase : std::enable_shared_from_this<SharedBase> {
  virtual ~SharedBase() = default;

  std::weak_ptr<SharedBase> WeakBasePtr() const {
    // For some reason, `static_pointer_cast` doesn't work with weak_ptr.
    return const_cast<SharedBase*>(this)->weak_from_this();
  }

  template <class Self>
  std::weak_ptr<Self> WeakPtr(this const Self& self) {
    // For some reason, `static_pointer_cast` doesn't work with weak_ptr.
    return dynamic_pointer_cast<Self>(const_cast<Self&>(self).shared_from_this());
  }

  template <class Self>
  std::shared_ptr<Self> SharedPtr(this const Self& self) {
    // We have to use dynamic_pointer_cast because `SharedBase` is a virtual base class.
    // Meaning that depending on the Self type, the address of SharedBase may be different.
    // Removing virtual inheritance may allow us to use static_pointer_cast here.
    return dynamic_pointer_cast<Self>(const_cast<Self&>(self).shared_from_this());
  }
};

}  // namespace automat