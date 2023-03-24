#pragma once

#include <unordered_map>
#include <unordered_set>

namespace automaton {

struct dual_ptr_holder;

struct dual_ptr_base {
  virtual ~dual_ptr_base() = default;
  virtual void HolderDestroyed(dual_ptr_holder &) = 0;
};

struct dual_ptr_holder {
  std::unordered_set<dual_ptr_base*> ptrs;
  ~dual_ptr_holder() {
    for (auto ptr : ptrs) {
      ptr->HolderDestroyed(*this);
    }
  }
};

template<typename T>
struct dual_ptr : dual_ptr_base {
  std::unordered_map<dual_ptr_holder*, T> holders;

  ~dual_ptr() {
    for (auto& it : holders) {
      it.first->ptrs.erase(this);
    }
  }

  struct iterator {
    typename std::unordered_map<dual_ptr_holder*, T>::iterator it;
    T& operator*() {
      return it->second;
    }
    iterator& operator++() {
      ++it;
      return *this;
    }
    bool operator!=(const iterator& other) {
      return it != other.it;
    }
  };

  iterator begin() {
    return iterator(holders.begin());
  }

  iterator end() {
    return iterator(holders.end());
  }

  T& operator[](dual_ptr_holder& holder) {
    auto it = holders.find(&holder);
    if (it == holders.end()) {
      it = holders.emplace(&holder, T()).first;
      holder.ptrs.insert(this);
    }
    return it->second;
  }

  void HolderDestroyed(dual_ptr_holder& holder) override {
    holders.erase(&holder);
  }
};

} // namespace automaton