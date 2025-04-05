// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cassert>
#include <memory>  // IWYU pragma: keep

template <typename T>
struct ReferenceCounted {
  using AtomicCounter = std::atomic<uint32_t>;

  mutable AtomicCounter owning_refs = 1;
  mutable AtomicCounter weak_refs = 1;  // weak_refs = #weak + (1 if owning_refs > 0 else 0)

  virtual ~ReferenceCounted() = default;

  [[nodiscard]] bool IncrementOwningRefsNonZero() const {
    uint32_t n = owning_refs.load(std::memory_order_relaxed);
    while (n != 0) {
      if (owning_refs.compare_exchange_weak(n, n + 1, std::memory_order_acquire,
                                            std::memory_order_relaxed)) {
        return true;
      }
    }
    return false;
  }
  void IncrementOwningRefs() const { owning_refs.fetch_add(1, std::memory_order_relaxed); }
  void IncrementWeakRefs() const { weak_refs.fetch_add(1, std::memory_order_relaxed); }
  void DecrementOwningRefs() const {
    if (owning_refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      this->~ReferenceCounted();  // call destructor without releasing memory
      DecrementWeakRefs();
    }
  }
  void DecrementWeakRefs() const {
    if (weak_refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      // release memory without calling destructor
      T::operator delete((void*)this);
    }
  }
};

// Null-safe obj->IncrementOwningRefs()
template <typename T>
static inline T* SafeIncrementOwningRefs(T* obj) {
  if (obj) {
    obj->IncrementOwningRefs();
  }
  return obj;
}

// Null-safe obj->DecrementOwningRefs()
template <typename T>
static inline void SafeDecrementOwningRefs(T* obj) {
  if (obj) {
    obj->DecrementOwningRefs();
  }
}

// Null-safe obj->IncrementWeakRefs()
template <typename T>
static inline T* SafeIncrementWeakRefs(T* obj) {
  if (obj) {
    obj->IncrementWeakRefs();
  }
  return obj;
}

// Null-safe obj->DecrementWeakRefs()
template <typename T>
static inline void SafeDecrementWeakRefs(T* obj) {
  if (obj) {
    obj->DecrementWeakRefs();
  }
}

template <typename T>
struct [[clang::trivial_abi]] Ptr {
  using element_type = T;

  constexpr Ptr() : obj(nullptr) {}
  constexpr Ptr(std::nullptr_t) : obj(nullptr) {}

  // Duplicate `that` Ptr (incrementing its ref count).
  Ptr(const Ptr<T>& that) : obj(SafeIncrementOwningRefs(that.Get())) {}
  template <typename U,
            typename = typename std::enable_if<std::is_convertible<U*, T*>::value>::type>
  Ptr(const Ptr<U>& that) : obj(SafeIncrementOwningRefs(that.Get())) {}

  // Adopt the underlying object from another Ptr (clearing it).
  Ptr(Ptr<T>&& that) : obj(that.Release()) {}
  template <typename U,
            typename = typename std::enable_if<std::is_convertible<U*, T*>::value>::type>
  Ptr(Ptr<U>&& that) : obj(that.Release()) {}

  // Adopt the bare pointer into the newly created Ptr.
  explicit Ptr(T*&& obj) : obj(obj) {}

  ~Ptr() { SafeDecrementOwningRefs(obj); }

  Ptr<T>& operator=(std::nullptr_t) {
    this->Reset();
    return *this;
  }

  /**
   *  Shares the underlying object referenced by the argument by calling ref() on it. If this
   *  Ptr previously had a reference to an object (i.e. not null) it will call unref() on that
   *  object.
   */
  Ptr<T>& operator=(const Ptr<T>& that) {
    if (this != &that) {
      this->Reset(SafeIncrementOwningRefs(that.Get()));
    }
    return *this;
  }
  template <typename U,
            typename = typename std::enable_if<std::is_convertible<U*, T*>::value>::type>
  Ptr<T>& operator=(const Ptr<U>& that) {
    this->Reset(SafeIncrementOwningRefs(that.Get()));
    return *this;
  }

  /**
   *  Move the underlying object from the argument to the Ptr. If the Ptr previously held
   *  a reference to another object, unref() will be called on that object. No call to ref()
   *  will be made.
   */
  Ptr<T>& operator=(Ptr<T>&& that) {
    this->Reset(that.Release());
    return *this;
  }
  template <typename U,
            typename = typename std::enable_if<std::is_convertible<U*, T*>::value>::type>
  Ptr<T>& operator=(Ptr<U>&& that) {
    this->Reset(that.Release());
    return *this;
  }

  T& operator*() const {
    assert(this->Get() != nullptr);
    return *this->Get();
  }

  explicit operator bool() const { return this->Get() != nullptr; }

  T* Get() const { return obj; }
  T* operator->() const { return obj; }

  /**
   *  Adopt the new bare pointer, and call unref() on any previously held object (if not null).
   *  No call to ref() will be made.
   */
  void Reset(T* ptr = nullptr) {
    // Calling fPtr->unref() may call this->~() or this->reset(T*).
    // http://wg21.cmeerw.net/lwg/issue998
    // http://wg21.cmeerw.net/lwg/issue2262
    T* oldObj = obj;
    obj = ptr;
    SafeDecrementOwningRefs(oldObj);
  }

  /**
   *  Return the bare pointer, and set the internal object pointer to nullptr.
   *  The caller must assume ownership of the object, and manage its reference count directly.
   *  No call to unref() will be made.
   */
  [[nodiscard]] T* Release() {
    T* ptr = obj;
    obj = nullptr;
    return ptr;
  }

  void Swap(Ptr<T>& that) /*noexcept*/ {
    using std::swap;
    swap(obj, that.obj);
  }

 private:
  T* obj;
};

template <typename T>
inline void swap(Ptr<T>& a, Ptr<T>& b) /*noexcept*/ {
  a.Swap(b);
}

template <typename T, typename U>
inline bool operator==(const Ptr<T>& a, const Ptr<U>& b) {
  return a.Get() == b.Get();
}
template <typename T>
inline bool operator==(const Ptr<T>& a, std::nullptr_t) /*noexcept*/ {
  return !a;
}
template <typename T>
inline bool operator==(std::nullptr_t, const Ptr<T>& b) /*noexcept*/ {
  return !b;
}

template <typename T, typename U>
inline bool operator!=(const Ptr<T>& a, const Ptr<U>& b) {
  return a.Get() != b.Get();
}
template <typename T>
inline bool operator!=(const Ptr<T>& a, std::nullptr_t) /*noexcept*/ {
  return static_cast<bool>(a);
}
template <typename T>
inline bool operator!=(std::nullptr_t, const Ptr<T>& b) /*noexcept*/ {
  return static_cast<bool>(b);
}

template <typename C, typename CT, typename T>
auto operator<<(std::basic_ostream<C, CT>& os, const Ptr<T>& sp) -> decltype(os << sp.Get()) {
  return os << sp.Get();
}

template <typename T>
Ptr(T*) -> Ptr<T>;

template <typename T, typename... Args>
Ptr<T> MakePtr(Args&&... args) {
  return Ptr<T>(new T(std::forward<Args>(args)...));
}

/*
 *  Returns a Ptr wrapping the provided ptr AND calls ref on it (if not null).
 *
 *  This is different than the semantics of the constructor for Ptr, which just wraps the ptr,
 *  effectively "adopting" it.
 */
template <typename T>
Ptr<T> DupPtr(T* obj) {
  return Ptr<T>(SafeIncrementOwningRefs(obj));
}

template <typename T>
Ptr<T> DupPtr(const T* obj) {
  return Ptr<T>(const_cast<T*>(SafeIncrementOwningRefs(obj)));
}

// Add std::hash specialization for Ptr
namespace std {
template <typename T>
struct hash<Ptr<T>> {
  size_t operator()(const Ptr<T>& ptr) const { return std::hash<T*>()(ptr.Get()); }
};
}  // namespace std

// WeakPtr can holds a reference to a reference-counted object while also allowing that object to be
// destroyed. In order to use WeakPtr, it should first be converted to Ptr using `Lock().
template <typename T>
struct [[clang::trivial_abi]] WeakPtr {
  T* obj;

  WeakPtr(const Ptr<T>& ptr) : obj(SafeIncrementWeakRefs(ptr.Get())) {}
  WeakPtr(T* obj) : obj(SafeIncrementWeakRefs(obj)) {}
  WeakPtr(const WeakPtr<T>& that) : obj(SafeIncrementWeakRefs(that.obj)) {}
  WeakPtr(WeakPtr<T>&& that) : obj(that.ReleaseWeak()) {}

  ~WeakPtr() { SafeDecrementWeakRefs(obj); }

  // Clears this pointer and returns it's existing value. It's up to the caller to take care of
  // decrementing the weak reference count on the released pointer.
  [[nodiscard]] T* ReleaseWeak() {
    T* ptr = obj;
    obj = nullptr;
    return ptr;
  }

  bool IsExpired() const {
    if (obj) {
      return obj->owning_refs.load(std::memory_order_relaxed) == 0;
    }
    return true;
  }

  Ptr<T> Lock() const {
    if (obj && obj->IncrementOwningRefsNonZero()) {
      return Ptr<T>(obj);
    }
    return Ptr<T>();
  }

  WeakPtr<T>& operator=(const WeakPtr<T>& that) {
    if (this != &that) {
      T* oldObj = obj;
      obj = SafeIncrementWeakRefs(that.obj);
      SafeDecrementWeakRefs(oldObj);
    }
    return *this;
  }

  WeakPtr<T>& operator=(WeakPtr<T>&& that) {
    T* oldObj = obj;
    obj = that.ReleaseWeak();
    SafeDecrementWeakRefs(oldObj);
    return *this;
  }

  void Reset(T* ptr = nullptr) {
    T* oldObj = obj;
    obj = SafeIncrementWeakRefs(ptr);
    SafeDecrementWeakRefs(oldObj);
  }
};
