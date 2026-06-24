#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include <functional>
#include <type_traits>
#include <utility>

namespace automat {

// Non-owning, type-erased reference to a callable. The referent must outlive the FnRef.
template <class Sig>
class FnRef;

template <class R, class... Args>
class FnRef<R(Args...)> {
 public:
  FnRef() = default;
  FnRef(std::nullptr_t) {}

  template <class F>
    requires(!std::same_as<std::remove_cvref_t<F>, FnRef> && std::is_invocable_r_v<R, F&, Args...>)
  FnRef(F&& f)
      : obj_(ObjectPointer(std::addressof(f))), thunk_(+[](void* obj, Args... args) -> R {
          using Referent = std::remove_reference_t<F>;
          if constexpr (std::is_function_v<Referent>) {
            return std::invoke(reinterpret_cast<Referent*>(obj), std::forward<Args>(args)...);
          } else {
            return std::invoke(*static_cast<Referent*>(obj), std::forward<Args>(args)...);
          }
        }) {}

  R operator()(Args... args) const { return thunk_(obj_, std::forward<Args>(args)...); }

  explicit operator bool() const { return thunk_ != nullptr; }

 private:
  template <class T>
  static void* ObjectPointer(T* p) {
    if constexpr (std::is_function_v<T>) {
      return reinterpret_cast<void*>(p);
    } else {
      return const_cast<void*>(static_cast<const void*>(p));
    }
  }

  void* obj_ = nullptr;
  R (*thunk_)(void*, Args...) = nullptr;
};

static_assert(sizeof(FnRef<void()>) == 16);

}  // namespace automat
