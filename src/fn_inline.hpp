#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include <cassert>
#include <cstddef>
#include <functional>
#include <type_traits>
#include <utility>

namespace automat {

// Owning callable with fixed inline storage. No heap allocation; the callable must fit Capacity and
// Align (enforced with static_assert at construction).
template <class Sig, size_t Capacity = 40, size_t Align = alignof(std::max_align_t)>
class FnInline;

template <class R, class... Args, size_t Capacity, size_t Align>
class FnInline<R(Args...), Capacity, Align> {
  struct VTable {
    R (*invoke)(const void*, Args...);
    void (*copy)(void*, const void*);
    void (*move)(void*, void*);
    void (*destroy)(void*);
  };

  template <class F>
  static const VTable* VTableFor() {
    static const VTable vtable = {
        [](const void* storage, Args... args) -> R {
          return std::invoke(*static_cast<F*>(const_cast<void*>(storage)),
                             std::forward<Args>(args)...);
        },
        [](void* dst, const void* src) { new (dst) F(*static_cast<const F*>(src)); },
        [](void* dst, void* src) { new (dst) F(std::move(*static_cast<F*>(src))); },
        [](void* storage) { static_cast<F*>(storage)->~F(); },
    };
    return &vtable;
  }

 public:
  FnInline() noexcept = default;
  FnInline(std::nullptr_t) noexcept {}

  template <class F>
    requires(!std::is_base_of_v<FnInline, std::remove_cvref_t<F>> &&
             std::is_invocable_r_v<R, std::remove_cvref_t<F>&, Args...>)
  FnInline(F&& f) {
    using Callable = std::remove_cvref_t<F>;
    static_assert(sizeof(Callable) <= Capacity, "callable too large for FnInline capacity");
    static_assert(alignof(Callable) <= Align, "callable over-aligned for FnInline");
    new (&storage_) Callable(std::forward<F>(f));
    vtable_ = VTableFor<Callable>();
  }

  FnInline(const FnInline& other) {
    if (other.vtable_) {
      other.vtable_->copy(&storage_, &other.storage_);
      vtable_ = other.vtable_;
    }
  }

  FnInline(FnInline&& other) noexcept {
    if (other.vtable_) {
      other.vtable_->move(&storage_, &other.storage_);
      vtable_ = other.vtable_;
      other.Reset();
    }
  }

  FnInline& operator=(const FnInline& other) {
    if (this != &other) {
      Reset();
      if (other.vtable_) {
        other.vtable_->copy(&storage_, &other.storage_);
        vtable_ = other.vtable_;
      }
    }
    return *this;
  }

  FnInline& operator=(FnInline&& other) noexcept {
    if (this != &other) {
      Reset();
      if (other.vtable_) {
        other.vtable_->move(&storage_, &other.storage_);
        vtable_ = other.vtable_;
        other.Reset();
      }
    }
    return *this;
  }

  FnInline& operator=(std::nullptr_t) noexcept {
    Reset();
    return *this;
  }

  ~FnInline() { Reset(); }

  R operator()(Args... args) const {
    assert(vtable_ != nullptr);
    return vtable_->invoke(&storage_, std::forward<Args>(args)...);
  }

  explicit operator bool() const noexcept { return vtable_ != nullptr; }

  void Reset() noexcept {
    if (vtable_) {
      vtable_->destroy(&storage_);
      vtable_ = nullptr;
    }
  }

 private:
  alignas(Align) std::byte storage_[Capacity];
  const VTable* vtable_ = nullptr;
};

static_assert(sizeof(FnInline<void(), 40>) == 48);

}  // namespace automat
