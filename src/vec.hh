#pragma once

#include <vector>

#include "span.hh"

namespace maf {

template <typename T = char>
struct Vec : std::vector<T> {
  using std::vector<T>::vector;
  using iterator = typename std::vector<T>::iterator;

  Span<> Span() { return {this->data(), this->size()}; }

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
};

}  // namespace maf