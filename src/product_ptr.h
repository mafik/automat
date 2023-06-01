#pragma once

#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace automat {

struct product_holder;

struct product_ptr_base {
  virtual ~product_ptr_base() = default;
  virtual void HolderDestroyed(product_holder &) = 0;
};

// An object that can be used as a key for accessing objects held by
// `product_ptr`. When it's destroyed, the data stored in product_ptrs, which
// is indexed by this product_holder, is also destroyed.
struct product_holder {
  std::unordered_set<product_ptr_base *> ptrs;
  ~product_holder() {
    for (auto ptr : ptrs) {
      ptr->HolderDestroyed(*this);
    }
  }
};

template <typename T> struct construct_functor {
  T operator()() { return T(); }
};

// Product pointers allow objects to be owned by a Cartesian product of two
// classes of objects.
//
// Cartesian product: https://en.wikipedia.org/wiki/Cartesian_product
//
// For example to grade students in each course it's possible to use the
// following code:
//
// class Student {
//   product_ptr<int> grades;
// };
//
// class Course {
//   product_holder holder;
// };
//
// This code is sufficient to store grades for each student in each course.
// A grade can be accessed with: `grades[course.holder]`. When either Student
// of Grade is destroyed, their corresponding grades are also destroyed.
template <typename T, typename C = construct_functor<T>>
struct product_ptr : product_ptr_base {
  std::unordered_map<product_holder *, T> holders;

  ~product_ptr() {
    for (auto &it : holders) {
      it.first->ptrs.erase(this);
    }
  }

  struct iterator {
    typename std::unordered_map<product_holder *, T>::iterator it;
    T &operator*() { return it->second; }
    iterator &operator++() {
      ++it;
      return *this;
    }
    bool operator!=(const iterator &other) { return it != other.it; }
  };

  iterator begin() { return iterator(holders.begin()); }

  iterator end() { return iterator(holders.end()); }

  T &operator[](product_holder &holder) {
    auto it = holders.find(&holder);
    if (it == holders.end()) {
      C c;
      it = holders.emplace(&holder, c()).first;
      holder.ptrs.insert(this);
    }
    return it->second;
  }

  T &GetOrCreate(product_holder &holder, std::function<T()> &create) {
    auto it = holders.find(&holder);
    if (it == holders.end()) {
      it = holders.emplace(&holder, create()).first;
      holder.ptrs.insert(this);
    }
    return it->second;
  }

  void HolderDestroyed(product_holder &holder) override {
    holders.erase(&holder);
  }
};

} // namespace automat