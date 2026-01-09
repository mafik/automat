// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cassert>
#include <compare>
#include <memory>  // IWYU pragma: keep

#include "part.hh"

namespace automat {

template <typename T>
concept HasOperatorDelete = requires { T::operator delete((void*)nullptr); };

struct TrackedPtrBase;

template <typename T>
struct TrackedPtr;

// Base class for objects that want synchronous single-threaded reference tracking through
// TrackedPtr<T>.
struct Trackable : virtual Part {
  TrackedPtrBase* ref_list = nullptr;

  virtual ~Trackable();

  template <typename Self>
  auto AcquireTrackedPtr(this Self& self) -> TrackedPtr<Self> {
    return TrackedPtr<Self>(&self);
  }
};

// Safe (weak) reference to Widget. Automatically set to nullptr when the widget is destroyed.
struct TrackedPtrBase {
  TrackedPtrBase* next = nullptr;
  Trackable* trackable = nullptr;
  TrackedPtrBase() {}
  TrackedPtrBase(Trackable* t) { Reset(t); }
  TrackedPtrBase(const TrackedPtrBase& other) : TrackedPtrBase() { Reset(other.trackable); }
  TrackedPtrBase& operator=(const TrackedPtrBase& that) {
    Reset(that.trackable);
    return *this;
  }
  ~TrackedPtrBase() { Reset(); }

  operator bool() const { return trackable != nullptr; }
  void Reset(Trackable* new_trackable = nullptr) {
    if (new_trackable == trackable) {
      return;
    }
    if (trackable) {
      if (trackable->ref_list == this) {
        trackable->ref_list = next;
      } else {
        TrackedPtrBase* prev = trackable->ref_list;
        while (prev->next != this) {
          prev = prev->next;
        }
        prev->next = next;
      }
    }
    if (new_trackable) {
      // LOG << "new_trackable: " << f("{}", (void*)new_trackable);
      // LOG << "trackable: " << f("{}", (void*)trackable);
      next = new_trackable->ref_list;
      new_trackable->ref_list = this;
      trackable = new_trackable;
    } else {
      trackable = nullptr;
    }
  }
};

inline Trackable::~Trackable() {
  TrackedPtrBase* ref = ref_list;
  while (ref) {
    ref->trackable = nullptr;
    ref = ref->next;
  }
}

template <typename T>
struct TrackedPtr : TrackedPtrBase {
  TrackedPtr() : TrackedPtrBase() {}
  TrackedPtr(std::nullptr_t) : TrackedPtrBase() {}
  template <typename U>
    requires std::convertible_to<U*, T*>
  TrackedPtr(U* t) : TrackedPtrBase(t) {}
  template <typename U>
    requires std::convertible_to<U*, T*>
  TrackedPtr(const TrackedPtr<U>& other) : TrackedPtrBase(other) {}

  template <typename U>
    requires std::convertible_to<U*, T*>
  TrackedPtr<T>& operator=(const TrackedPtr<U>& that) {
    Reset(that.trackable);
    return *this;
  }
  template <typename U>
    requires std::convertible_to<U*, T*>
  TrackedPtr<T>& operator=(U* that) {
    Reset(that);
    return *this;
  }
  TrackedPtr<T>& operator=(std::nullptr_t) {
    Reset();
    return *this;
  }
  T* operator->() const { return static_cast<T*>(trackable); }
  T& operator*() const { return *static_cast<T*>(trackable); }
  operator T*() const { return static_cast<T*>(trackable); }
  T* Get() const { return static_cast<T*>(trackable); }
  T* get() const { return static_cast<T*>(trackable); }
};

template <typename T>
struct WeakPtr;

template <typename T>
struct Ptr;

struct ReferenceCounted : virtual Part {
  using AtomicCounter = std::atomic<uint32_t>;

  mutable AtomicCounter owning_refs = 1;
  mutable AtomicCounter weak_refs = 1;  // weak_refs = #weak + (1 if owning_refs > 0 else 0)

  ReferenceCounted() = default;
  ReferenceCounted(const ReferenceCounted&) : owning_refs(1), weak_refs(1) {}
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

  template <class Self>
  void DecrementOwningRefs(this const Self& self) {
    if (self.owning_refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      self.~ReferenceCounted();  // call destructor without releasing memory
      self.DecrementWeakRefs();
    }
  }
  template <class Self>
  void DecrementWeakRefs(this const Self& self) {
    if (self.weak_refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      // release memory without calling destructor
      if constexpr (HasOperatorDelete<Self>) {
        Self::operator delete((void*)&self);
      } else {
        operator delete((void*)&self);
      }
    }
  }

  template <class Self>
  WeakPtr<Self> AcquireWeakPtr(this const Self& self) {
    return WeakPtr<Self>(&const_cast<Self&>(self));
  }

  template <class Self>
  Ptr<Self> AcquirePtr(this const Self& self) {
    return Ptr<Self>(SafeIncrementOwningRefs(&const_cast<Self&>(self)));
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
struct [[clang::trivial_abi]] PtrBase {
  using element_type = T;

  template <class Self>
    requires requires(T t) { t.Reset(); }
  Self& operator=(this Self& self, std::nullptr_t) {
    self.Reset();
    return self;
  }

  explicit operator bool() const { return obj != nullptr; }

  // Reset & return the stored pointer. It's up to the caller to decrement the reference
  // count on the returned pointer.
  [[nodiscard]] T* Release() {
    T* ptr = obj;
    obj = nullptr;
    return ptr;
  }

  bool operator==(const T* that) const { return this->obj == that; }
  bool operator==(const PtrBase<T>& that) const { return this->obj == that.obj; }

 protected:
  constexpr PtrBase() : obj(nullptr) {}
  constexpr PtrBase(std::nullptr_t) : obj(nullptr) {}

  explicit PtrBase(T* obj) : obj(obj) {}

  template <typename U>
    requires std::convertible_to<U*, T*>
  explicit PtrBase(U* obj) : obj(obj) {}

  T* obj;
};

template <typename T>
struct [[clang::trivial_abi]] Ptr : PtrBase<T> {
  constexpr Ptr() : PtrBase<T>(nullptr) {}
  constexpr Ptr(std::nullptr_t) : PtrBase<T>(nullptr) {}

  // Duplicate `that` Ptr (incrementing its ref count).
  Ptr(const Ptr<T>& that) : PtrBase<T>(SafeIncrementOwningRefs(that.Get())) {}
  template <typename U>
    requires std::convertible_to<U*, T*>
  Ptr(const Ptr<U>& that) : PtrBase<T>(SafeIncrementOwningRefs(that.Get())) {}

  // Adopt the underlying object from another Ptr (clearing it).
  Ptr(Ptr<T>&& that) : PtrBase<T>(that.Release()) {}
  template <typename U>
    requires std::convertible_to<U*, T*>
  Ptr(Ptr<U>&& that) : PtrBase<T>(that.Release()) {}

  // Adopt the bare pointer into the newly created Ptr.
  explicit Ptr(T* obj) : PtrBase<T>(obj) {}

  ~Ptr() { SafeDecrementOwningRefs(this->obj); }

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
  template <typename U>
    requires std::convertible_to<U*, T*>
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
  template <typename U>
    requires std::convertible_to<U*, T*>
  Ptr<T>& operator=(Ptr<U>&& that) {
    this->Reset(that.Release());
    return *this;
  }

  T& operator*() const {
    assert(this->Get() != nullptr);
    return *this->Get();
  }

  T* get() const { return Get(); }

  template <typename U = T>
  U* Get() const {
    return static_cast<U*>(this->obj);
  }

  T* operator->() const { return this->obj; }

  std::strong_ordering operator<=>(const Ptr<T>& that) const { return this->obj <=> that.obj; }

  /**
   *  Adopt the new bare pointer, and call unref() on any previously held object (if not null).
   *  No call to ref() will be made.
   */
  void Reset(T* ptr = nullptr) {
    // Calling fPtr->unref() may call this->~() or this->reset(T*).
    // http://wg21.cmeerw.net/lwg/issue998
    // http://wg21.cmeerw.net/lwg/issue2262
    T* oldObj = this->obj;
    this->obj = ptr;
    SafeDecrementOwningRefs(oldObj);
  }
  void reset(T* ptr = nullptr) { Reset(ptr); }

  void Swap(Ptr<T>& that) {
    using std::swap;
    swap(this->obj, that.obj);
  }

  template <typename U>
  [[nodiscard]] Ptr<U> Cast(this auto&& self) {
    return Ptr<U>(static_cast<U*>(self.Release()));
  }
};

template <typename T>
inline void swap(Ptr<T>& a, Ptr<T>& b) {
  a.Swap(b);
}

template <typename T>
inline bool operator==(const Ptr<T>& a, std::nullptr_t) {
  return !a;
}
template <typename T>
inline bool operator==(std::nullptr_t, const Ptr<T>& b) {
  return !b;
}

template <typename T, typename U>
inline bool operator!=(const Ptr<T>& a, const Ptr<U>& b) {
  return a.Get() != b.Get();
}
template <typename T>
inline bool operator!=(const Ptr<T>& a, std::nullptr_t) {
  return static_cast<bool>(a);
}
template <typename T>
inline bool operator!=(std::nullptr_t, const Ptr<T>& b) {
  return static_cast<bool>(b);
}

template <typename C, typename CT, typename T>
auto operator<<(std::basic_ostream<C, CT>& os, const Ptr<T>& sp) -> decltype(os << sp.Get()) {
  return os << sp.Get();
}

// Create a new instance of T, wrapped in Ptr<T>.
//
// Note: this could be replaced with a "perfect forwarding" wrapper (similar to std::make_shared)
// but unfortunately it prevents clangd from properly inferring the argument types and producing
// warnings. For development convenience, it's better to keep it as a macro.
#define MAKE_PTR(T, ...) Ptr(new T(__VA_ARGS__))

// WeakPtr can holds a reference to a reference-counted object while also allowing that object to
// be destroyed. In order to use WeakPtr, it should first be converted to Ptr using `Lock().
template <typename T>
struct [[clang::trivial_abi]] WeakPtr : PtrBase<T> {
  WeakPtr() : PtrBase<T>(nullptr) {}
  WeakPtr(const Ptr<T>& ptr) : PtrBase<T>(SafeIncrementWeakRefs(ptr.Get())) {}
  WeakPtr(T* obj) : PtrBase<T>(SafeIncrementWeakRefs(obj)) {}

  // Copy constructors
  WeakPtr(const WeakPtr<T>& that) : PtrBase<T>(SafeIncrementWeakRefs(that.obj)) {}
  template <typename U>
    requires std::convertible_to<U*, T*>
  WeakPtr(const WeakPtr<U>& that) : PtrBase<T>(SafeIncrementWeakRefs(that.obj)) {}

  // Move constructors
  WeakPtr(WeakPtr<T>&& that) : PtrBase<T>(that.Release()) {}
  template <typename U>
    requires std::convertible_to<U*, T*>
  WeakPtr(WeakPtr<U>&& that) : PtrBase<T>(that.Release()) {}

  ~WeakPtr() { SafeDecrementWeakRefs(this->obj); }

  bool IsExpired() const {
    if (this->obj) {
      return this->obj->owning_refs.load(std::memory_order_relaxed) == 0;
    }
    return true;
  }

  Ptr<T> Lock() const {
    if (this->obj && this->obj->IncrementOwningRefsNonZero()) {
      return Ptr<T>(this->obj);
    }
    return Ptr<T>();
  }
  Ptr<T> lock() const { return Lock(); }  // alias for better compatibility with std::weak_ptr

  WeakPtr<T>& operator=(const WeakPtr<T>& that) {
    if (this != &that) {
      T* oldObj = this->obj;
      this->obj = SafeIncrementWeakRefs(that.obj);
      SafeDecrementWeakRefs(oldObj);
    }
    return *this;
  }

  WeakPtr<T>& operator=(WeakPtr<T>&& that) {
    T* newObj = that.Release();
    T* oldObj = this->obj;
    this->obj = newObj;
    SafeDecrementWeakRefs(oldObj);
    return *this;
  }

  template <typename U>
  [[nodiscard]] WeakPtr<U> Cast() && {
    return WeakPtr<U>(static_cast<U*>(this->Release()));
  }

  std::strong_ordering operator<=>(const WeakPtr<T>& that) const { return this->obj <=> that.obj; }

  void Reset(T* ptr = nullptr) {
    T* oldObj = this->obj;
    this->obj = SafeIncrementWeakRefs(ptr);
    SafeDecrementWeakRefs(oldObj);
  }

  template <typename U = T>
  U* GetUnsafe() const {
    return static_cast<U*>(this->obj);
  }

  template <class U>
  friend class WeakPtr;
};

template <typename T>
struct [[clang::trivial_abi]] NestedWeakPtr;

template <typename T>
struct [[clang::trivial_abi]] NestedPtr {
  NestedPtr() : ptr{}, obj(nullptr) {}
  NestedPtr(Ptr<ReferenceCounted>&& ptr, T* obj) : ptr(std::move(ptr)), obj(obj) {}

  template <typename U>
    requires std::convertible_to<U*, T*>
  NestedPtr(const Ptr<U>& that) : ptr(that), obj(that.Get()) {}

  template <typename U>
    requires std::convertible_to<U*, T*>
  NestedPtr(const NestedPtr<U>& that) : ptr(that.ptr), obj(that.obj) {}

  template <typename U>
    requires std::convertible_to<U*, T*>
  NestedPtr(NestedPtr<U>&& that) : ptr(std::move(that.ptr)), obj(that.obj) {}

  std::strong_ordering operator<=>(const NestedPtr<T>& that) const {
    return this->ptr <=> that.ptr;
  }
  T& operator*() const { return *obj; }
  T* operator->() const { return obj; }
  explicit operator bool() const { return obj != nullptr; }

  template <typename U>
  U* Owner() {
    return this->ptr.template Get<U>();
  }

  WeakPtr<ReferenceCounted> GetOwnerWeak() const { return ptr->AcquireWeakPtr(); }

  T* Get() const { return obj; }
  void Reset() {
    ptr.Reset();
    obj = nullptr;
  }

  template <typename U>
  [[nodiscard]] NestedPtr<U> DynamicCast(this auto&& self) {
    return NestedPtr<U>(Ptr<ReferenceCounted>(self.ptr), dynamic_cast<U*>(self.obj));
  }

 private:
  friend class NestedWeakPtr<T>;
  template <class U>
  friend class NestedPtr;
  Ptr<ReferenceCounted> ptr;
  T* obj;
};

template <typename T>
struct [[clang::trivial_abi]] NestedWeakPtr {
  NestedWeakPtr() : weak_ptr{}, obj(nullptr) {}
  NestedWeakPtr(const NestedPtr<T>& ptr) : weak_ptr(ptr.ptr), obj(ptr.obj) {}

  template <typename U>
    requires std::convertible_to<U*, ReferenceCounted*>
  NestedWeakPtr(const WeakPtr<U>& ptr, T* obj) : weak_ptr(ptr), obj(obj) {}

  NestedWeakPtr(WeakPtr<ReferenceCounted>&& ptr, T* obj) : weak_ptr(std::move(ptr)), obj(obj) {}

  std::strong_ordering operator<=>(const NestedWeakPtr<T>& that) const {
    return this->weak_ptr <=> that.weak_ptr;
  }
  bool operator==(const NestedWeakPtr<T>& that) const {
    return this->weak_ptr == that.weak_ptr && this->obj == that.obj;
  }
  explicit operator bool() const { return obj != nullptr; }
  void Reset() {
    weak_ptr.Reset();
    obj = nullptr;
  }
  NestedPtr<T> Lock() const {
    if (auto new_ptr = weak_ptr.Lock()) {
      return NestedPtr<T>{std::move(new_ptr), obj};
    }
    return NestedPtr<T>();
  }
  WeakPtr<ReferenceCounted> GetOwnerWeak() const { return weak_ptr; }

  template <typename U = T>
  U* GetUnsafe() const {
    return static_cast<U*>(obj);
  }

  template <typename U>
  U* OwnerUnsafe() const {
    return this->weak_ptr.template GetUnsafe<U>();
  }

 private:
  WeakPtr<ReferenceCounted> weak_ptr;
  T* obj;
};

using std::make_unique;
using std::unique_ptr;

}  // namespace automat

// Add std::hash specialization for Ptr
namespace std {
template <typename T>
struct hash<automat::Ptr<T>> {
  size_t operator()(const automat::Ptr<T>& ptr) const { return std::hash<T*>()(ptr.Get()); }
};
}  // namespace std
