// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <llvm/ADT/SmallVector.h>

#include <algorithm>
#include <vector>

#include "span.hh"

namespace automat {

template <typename T, unsigned N = llvm::CalculateSmallVectorDefaultInlinedElements<T>::value>
using SmallVec = llvm::SmallVector<T, N>;

template <typename T = char>
struct Vec : std::vector<T> {
  using std::vector<T>::vector;
  using iterator = typename std::vector<T>::iterator;

  operator Span<T>() { return {this->data(), this->size()}; }

  template <typename U>
  void Append(const U& u) {
    this->insert(this->end(), (T*)&u, (T*)&u + sizeof(U) / sizeof(T));
  }

  // Returns true if the vector contains the given value.
  //
  // Note: When C++23 is properly supported this could be replaced with
  // ranges::contains.
  bool Contains(const T& value) const {
    for (const auto& v : *this) {
      if (v == value) {
        return true;
      }
    }
    return false;
  }

  // Removes the first occurrence of the given value from the vector.
  //
  // Returns an iterator to the element after the removed element (which may be
  // `end()`).
  //
  // If the vector does not contain the given value, returns `end()`.
  iterator Erase(const T& value) {
    for (auto it = this->begin(); it != this->end(); ++it) {
      if (*it == value) {
        return this->erase(it);
      }
    }
    return this->end();
  }

  // Removes the element at the given index from the vector.
  //
  // Returns an iterator to the element after the removed element (which may be `end()`).
  iterator EraseIndex(int i) { return this->erase(this->begin() + i); }

  // Inserts `value` into a sorted vector such that the vector remains sorted.
  //
  // `proj` is called on each element to extract the key used for comparison.
  // The vector is expected to already be sorted by `proj(element)`.
  template <typename Key, typename Proj = std::identity>
  iterator InsertSorted(const Key& key, T value, Proj proj = {}) {
    auto pos = std::ranges::upper_bound(*this, key, {}, proj);
    return this->insert(pos, std::move(value));
  }
};

template <typename T, typename... Args>
Vec<T> MakeVec(Args&&... t) {
  Vec<T> vec;
  vec.reserve(sizeof...(Args));
  (vec.emplace_back(std::move(t)), ...);
  return vec;
}

template <typename T>
void FastRemove(std::vector<T>& vec, const T& value) {
  auto it = std::find(vec.begin(), vec.end(), value);
  if (it != vec.end()) {
    *it = std::move(vec.back());
    vec.pop_back();
  }
}

}  // namespace automat