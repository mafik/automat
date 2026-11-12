#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include <cstddef>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

#if !defined(__cpp_lib_move_only_function)

namespace std {

template <class Signature>
class move_only_function;

template <class R, class... Args>
class move_only_function<R(Args...)> {
  struct Callable {
    virtual ~Callable() = default;
    virtual R Call(Args&&...) = 0;
  };

  template <class F>
  struct CallableOf : Callable {
    F f;
    template <class G>
    CallableOf(G&& g) : f(std::forward<G>(g)) {}
    R Call(Args&&... args) override { return std::invoke(f, std::forward<Args>(args)...); }
  };

  unique_ptr<Callable> callable;

 public:
  move_only_function() noexcept = default;
  move_only_function(nullptr_t) noexcept {}

  template <class F>
    requires(!is_same_v<remove_cvref_t<F>, move_only_function>)
  move_only_function(F&& f) : callable(new CallableOf<decay_t<F>>(std::forward<F>(f))) {}

  R operator()(Args... args) { return callable->Call(std::forward<Args>(args)...); }

  explicit operator bool() const noexcept { return callable != nullptr; }
};

}  // namespace std

#endif
