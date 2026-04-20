// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <llvm/ADT/STLExtras.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <stop_token>

#include "fn.hh"
#include "int.hh"
#include "vec.hh"

namespace automat {

// Stores a value of a boolean in an atomic fashion, allowing multiple threads to safely block /
// wait and get notified when it changes.
struct ConcurrentBool {
  using Ticket = U32;

  void Set(bool new_value) {
    std::unique_lock lock(mutex_);
    bool old_value = value_.exchange(new_value, std::memory_order_release);
    if (old_value == new_value) {
      return;
    }
    // Copy callbacks so they can be invoked with the lock released. This lets
    // callbacks call back into this ConcurrentBool, and makes removal during
    // invocation safe (the in-flight copy still fires).
    SmallVec<Entry, 1> callbacks = callbacks_;
    lock.unlock();

    cv_.notify_all();

    for (auto& e : callbacks) {
      if (e.kind == Kind::Change || (e.kind == Kind::True && new_value) ||
          (e.kind == Kind::False && !new_value)) {
        e.fn(new_value);
      }
    }
  }

  bool Get() const { return value_.load(std::memory_order_acquire); }

  Ticket AddCallbackOnChange(Fn<void(bool)> callback) {
    return AddCallback(Kind::Change, std::move(callback));
  }
  void RemoveCallbackOnChange(Ticket ticket) { RemoveCallback(Kind::Change, ticket); }

  Ticket AddCallbackOnTrue(Fn<void()> callback) {
    return AddCallback(Kind::True, [fn = std::move(callback)](bool) { fn(); });
  }
  void RemoveCallbackOnTrue(Ticket ticket) { RemoveCallback(Kind::True, ticket); }

  Ticket AddCallbackOnFalse(Fn<void()> callback) {
    return AddCallback(Kind::False, [fn = std::move(callback)](bool) { fn(); });
  }
  void RemoveCallbackOnFalse(Ticket ticket) { RemoveCallback(Kind::False, ticket); }

  // Blocks the current thread until the value satisfies the predicate.
  // Returns true if the predicate was satisfied, false if stop was requested first.
  template <typename Predicate>
  bool Wait(Predicate pred, std::stop_token stop_token) {
    std::unique_lock lock(mutex_);
    return cv_.wait(lock, stop_token,
                    [&] { return pred(value_.load(std::memory_order_acquire)); });
  }

  bool WaitTrue(std::stop_token stop_token) {
    return Wait([](bool value) { return value; }, stop_token);
  }

  bool WaitFalse(std::stop_token stop_token) {
    return Wait([](bool value) { return !value; }, stop_token);
  }

 private:
  enum class Kind : U8 { Change, True, False };
  struct Entry {
    Ticket ticket;
    Kind kind;
    Fn<void(bool)> fn;
  };

  Ticket AddCallback(Kind kind, Fn<void(bool)> fn) {
    std::lock_guard lock(mutex_);
    Ticket t = ++next_ticket_;
    callbacks_.push_back({t, kind, std::move(fn)});
    return t;
  }
  void RemoveCallback(Kind kind, Ticket ticket) {
    std::lock_guard lock(mutex_);
    llvm::erase_if(callbacks_,
                   [&](const Entry& e) { return e.kind == kind && e.ticket == ticket; });
  }

  mutable std::mutex mutex_;
  std::condition_variable_any cv_;
  std::atomic<bool> value_{false};
  Ticket next_ticket_ = 0;
  SmallVec<Entry, 1> callbacks_;
};

}  // namespace automat
