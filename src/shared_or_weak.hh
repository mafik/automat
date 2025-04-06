// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "ptr.hh"

namespace automat {

template <typename T>
struct SharedOrWeakPtr {
  bool is_shared;  // this is using 8 bytes :(
  union {
    Ptr<T> shared;
    WeakPtr<T> weak;
  };

  SharedOrWeakPtr() : is_shared(true), shared() {}
  SharedOrWeakPtr(Ptr<T>&& shared) : is_shared(true), shared(std::move(shared)) {}
  ~SharedOrWeakPtr() { reset(); }

  SharedOrWeakPtr& operator=(Ptr<T>&& new_shared) {
    if (!is_shared) {
      weak.Reset();
      is_shared = true;
    }
    shared = std::move(new_shared);
    return *this;
  }

  SharedOrWeakPtr& operator=(WeakPtr<T>&& new_weak) {
    if (is_shared) {
      is_shared = false;
      shared.Reset();
    }
    weak = std::move(new_weak);
    return *this;
  }

  // Return the value of the pointer but only if SharedOrWeakPtr owns a shared pointer.
  // If SharedOrWeakPtr owns only a weak pointer, returns nullptr.
  // Use `lock()` to get the pointer value if you need to access it through the weak pointer.
  T* get() const {
    if (is_shared) {
      return shared.Get();
    } else {
      return nullptr;
    }
  }

  // Convert the SharedOrWeakPtr to a weak pointer and return the shared pointer.
  Ptr<T> borrow() {
    if (is_shared) {
      auto ret = std::move(shared);
      weak = ret;
      is_shared = false;
      return ret;
    } else {
      return nullptr;
    }
  }

  Ptr<T> lock() const {
    if (is_shared) {
      return shared;
    } else {
      return weak.Lock();
    }
  }

  void reset() {
    if (is_shared) {
      shared.Reset();
    } else {
      weak.Reset();
    }
  }

  bool operator==(std::nullptr_t) {
    if (is_shared) {
      return shared == nullptr;
    } else {
      return weak.IsExpired();
    }
  }
};
}  // namespace automat