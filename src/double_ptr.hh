#pragma once

#include "control_flow.hh"
#include "fn.hh"

namespace maf {

struct DoublePtrBase;

struct DoublePtrValueBase {
  const DoublePtrBase* owner_a;
  const DoublePtrBase* owner_b;
  DoublePtrValueBase(const DoublePtrBase* owner_a, const DoublePtrBase* owner_b)
      : owner_a(owner_a), owner_b(owner_b) {}
  virtual ~DoublePtrValueBase() = default;
};

// OPTIMIZATION: replace this vector with a couple hashmaps Or maybe a bunch of vectors (one per
// type)
extern std::vector<DoublePtrValueBase*> double_ptr_buffers;

// This holds a single value that is shared between two DoublePtrs. The value is held inline and is
// managed by the lifetime of the DoublePtrs. Don't use this class directly. Instead use DoublePtr.
template <typename T>
struct DoublePtrValue : DoublePtrValueBase {
  T value;
  DoublePtrValue(const DoublePtrBase* owner_a, const DoublePtrBase* owner_b, T value)
      : DoublePtrValueBase(owner_a, owner_b), value(value) {}
  ~DoublePtrValue() override = default;

  // Call this to
  void Release() {
    for (auto it = double_ptr_buffers.begin(); it != double_ptr_buffers.end();) {
      if (*it == this) {
        if (*it != double_ptr_buffers.back()) {
          *it = double_ptr_buffers.back();
        }
        double_ptr_buffers.pop_back();
        delete this;
        return;
      } else {
        ++it;
      }
    }
  }
};

struct DoublePtrBase {
  virtual ~DoublePtrBase() {
    for (auto it = double_ptr_buffers.begin(); it != double_ptr_buffers.end();) {
      auto* buffer = static_cast<DoublePtrValueBase*>(*it);
      if (buffer->owner_a == this || buffer->owner_b == this) {
        if (*it != double_ptr_buffers.back()) {
          *it = double_ptr_buffers.back();
        }
        double_ptr_buffers.pop_back();
        delete buffer;
      } else {
        ++it;
      }
    }
  }
};

template <typename T>
struct DoublePtr : DoublePtrBase {
  DoublePtr() {}

  // forbid copy and move
  DoublePtr(const DoublePtr&) = delete;
  DoublePtr& operator=(const DoublePtr&) = delete;
  DoublePtr(DoublePtr&&) = delete;

  // Find and return value owned by this & other ptr
  DoublePtrValue<T>* Find(const DoublePtrBase& other) const {
    for (auto* ptr : double_ptr_buffers) {
      auto* buffer = static_cast<DoublePtrValue<T>*>(ptr);
      if ((buffer->owner_a == this && buffer->owner_b == &other) ||
          (buffer->owner_a == &other && buffer->owner_b == this)) {
        return buffer;
      }
    }
    return nullptr;
  }

  // Try to find a value owned by this & other ptr, if not found, make it using the provided
  // function
  DoublePtrValue<T>& FindOrMake(const DoublePtrBase& other, maf::Fn<T()> make) const {
    if (auto* value = Find(other)) {
      return *value;
    }
    auto* buffer = new DoublePtrValue<T>(this, &other, make());
    double_ptr_buffers.push_back(buffer);
    return *buffer;
  }

  T& operator[](const DoublePtrBase& other) const {
    return FindOrMake(other, [] { return T{}; }).value;
  }

  void ForEach(maf::Fn<automat::ControlFlow(DoublePtrValue<T>&)> fn) {
    for (auto* ptr : double_ptr_buffers) {
      auto* buffer = static_cast<DoublePtrValue<T>*>(ptr);
      if (buffer->owner_a == this || buffer->owner_b == this) {
        if (fn(buffer) == automat::ControlFlow::Stop) {
          break;
        }
      }
    }
  }

  struct end_iterator {
    bool operator!=(const end_iterator&) { return false; }
  };

  struct iterator {
    DoublePtr& ptr;
    size_t index = 0;
    iterator(DoublePtr& ptr) : ptr(ptr) {
      while (index < double_ptr_buffers.size() &&
             (static_cast<DoublePtrValue<T>*>(double_ptr_buffers[index])->owner_a != &ptr &&
              static_cast<DoublePtrValue<T>*>(double_ptr_buffers[index])->owner_b != &ptr)) {
        ++index;
      }
    }
    T& operator*() { return static_cast<DoublePtrValue<T>*>(double_ptr_buffers[index])->value; }
    iterator& operator++() {
      do {
        ++index;
      } while (index < double_ptr_buffers.size() &&
               (static_cast<DoublePtrValue<T>*>(double_ptr_buffers[index])->owner_a != &ptr &&
                static_cast<DoublePtrValue<T>*>(double_ptr_buffers[index])->owner_b != &ptr));
      return *this;
    }
    bool operator!=(const iterator& other) { return index != other.index; }
    bool operator!=(const end_iterator&) { return index < double_ptr_buffers.size(); }
  };

  iterator begin() { return iterator(*this); }
  end_iterator end() { return end_iterator{}; }
};

}  // namespace maf
