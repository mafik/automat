// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <functional>

// Shortcut for std::function
namespace automat {

template <typename T>
using Fn = std::function<T>;

template <typename T>
struct FnIs {
  const T* bare_ptr;
  FnIs(T* bare_ptr) : bare_ptr(bare_ptr) {}

  bool operator()(const Fn<T>& fn) const {
    T* const* fn_ptr_ptr = fn.template target<T*>();
    return (fn_ptr_ptr != nullptr) && (*fn_ptr_ptr == bare_ptr);
  }
};

}  // namespace automat