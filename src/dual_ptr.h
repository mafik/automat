#pragma once

#include <unordered_map>
#include <unordered_set>

namespace automaton {

struct dual_ptr_holder;

struct dual_ptr_base {
  virtual ~dual_ptr_base() = default;
  virtual void HolderDestroyed(dual_ptr_holder &) = 0;
};

// An object that co-owns a bunch of dual_ptrs. When it is destroyed, the data
// stored in dual_ptrs, which is indexed by this dual_ptr_holder, is also
// destroyed.
struct dual_ptr_holder {
  std::unordered_set<dual_ptr_base *> ptrs;
  ~dual_ptr_holder() {
    for (auto ptr : ptrs) {
      ptr->HolderDestroyed(*this);
    }
  }
};

// A pointer that can point to multiple objects at once (keyed by
// dual_ptr_holder).
//
// For example `dual_ptr` allows animated objects to store their animation state
// in the window that displays the animation. Window doesn't need to know about
// the objects themselves. When either the window or the animated object is
// destroyed, the objects held by `dual_ptr` are also destroyed.
template <typename T> struct dual_ptr : dual_ptr_base {
  std::unordered_map<dual_ptr_holder *, T> holders;

  ~dual_ptr() {
    for (auto &it : holders) {
      it.first->ptrs.erase(this);
    }
  }

  struct iterator {
    typename std::unordered_map<dual_ptr_holder *, T>::iterator it;
    T &operator*() { return it->second; }
    iterator &operator++() {
      ++it;
      return *this;
    }
    bool operator!=(const iterator &other) { return it != other.it; }
  };

  iterator begin() { return iterator(holders.begin()); }

  iterator end() { return iterator(holders.end()); }

  T &operator[](dual_ptr_holder &holder) {
    auto it = holders.find(&holder);
    if (it == holders.end()) {
      it = holders.emplace(&holder, T()).first;
      holder.ptrs.insert(this);
    }
    return it->second;
  }

  void HolderDestroyed(dual_ptr_holder &holder) override {
    holders.erase(&holder);
  }
};

} // namespace automaton