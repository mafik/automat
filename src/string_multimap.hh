// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

// This header provides a multimap which uses std::string as the key but can
// also be efficiently queried using std::string_view.

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace automat {

struct string_equal {
  using is_transparent = std::true_type;

  bool operator()(std::string_view l, std::string_view r) const noexcept { return l == r; }
};

struct string_hash {
  using is_transparent = std::true_type;

  auto operator()(std::string_view str) const noexcept {
    return std::hash<std::string_view>()(str);
  }
};

template <typename Value>
using string_multimap = std::unordered_multimap<std::string, Value, string_hash, string_equal>;

// Multimap functors, similar to string_equal & string_hash, but which allow
// interchangeable use of raw pointers and std::unique_ptr.
//
// Currently unused.
/*
template <typename T> struct ptr_equal {
  using is_transparent = void;

  template <typename LHS, typename RHS>
  auto operator()(const LHS &lhs, const RHS &rhs) const {
    return AsPtr(lhs) == AsPtr(rhs);
  }

private:
  static const T *AsPtr(const T *p) { return p; }
  static const T *AsPtr(const std::unique_ptr<T> &p) { return p.get(); }
};

template <typename T> struct ptr_hash {
  using is_transparent = void;

  auto operator()(T *p) const { return std::hash<T *>{}(p); }
  auto operator()(const std::unique_ptr<T> &p) const {
    return std::hash<T *>{}(p.get());
  }
};
*/

}  // namespace automat