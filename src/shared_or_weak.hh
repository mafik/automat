// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <memory>

namespace automat {

template <typename T>
struct SharedOrWeakPtr {
  bool is_shared;  // this is using 8 bytes :(
  union {
    std::shared_ptr<T> shared;
    std::weak_ptr<T> weak;
  };

  SharedOrWeakPtr() : is_shared(true), shared() {}
  SharedOrWeakPtr(std::shared_ptr<T>&& shared) : is_shared(true), shared(std::move(shared)) {}
  ~SharedOrWeakPtr() { reset(); }

  SharedOrWeakPtr& operator=(std::shared_ptr<T>&& new_shared) {
    if (!is_shared) {
      weak.reset();
      is_shared = true;
    }
    shared = std::move(new_shared);
    return *this;
  }

  SharedOrWeakPtr& operator=(std::weak_ptr<T>&& new_weak) {
    if (is_shared) {
      is_shared = false;
      shared.reset();
    }
    weak = std::move(new_weak);
    return *this;
  }

  // Return the value of the pointer but only if SharedOrWeakPtr owns a shared pointer.
  // If SharedOrWeakPtr owns only a weak pointer, returns nullptr.
  // Use `lock()` to get the pointer value if you need to access it through the weak pointer.
  T* get() const {
    if (is_shared) {
      return shared.get();
    } else {
      return nullptr;
    }
  }

  // Convert the SharedOrWeakPtr to a weak pointer and return the shared pointer.
  std::shared_ptr<T> borrow() {
    if (is_shared) {
      auto ret = std::move(shared);
      weak = ret;
      is_shared = false;
      return ret;
    } else {
      return nullptr;
    }
  }

  std::shared_ptr<T> lock() const {
    if (is_shared) {
      return shared;
    } else {
      return weak.lock();
    }
  }

  void reset() {
    if (is_shared) {
      shared.reset();
    } else {
      weak.reset();
    }
  }

  bool operator==(std::nullptr_t) {
    if (is_shared) {
      return shared == nullptr;
    } else {
      return weak.expired();
    }
  }
};
}  // namespace automat