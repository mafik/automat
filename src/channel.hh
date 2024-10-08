// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

// atomic is sometimes defined in <atomic> and sometimes in <memory>
#include <atomic>
#include <cassert>
#include <memory>

namespace automat {

// A channel allows one thread to send a value (by pointer) to another thread.
//
// Passing a value through a channel assumes that ownership is also transferred.
//
// A channel contains a one-element buffer so at least one value can be sent
// wait-free. Every subsequent send may block until the receiver has consumed
// the value.
//
// This channel implementation assumes a single consumer.
struct channel {
  std::atomic<void*> atomic{nullptr};

  // May block. `ptr` must not be `nullptr`.
  void send(void* ptr) {
    assert(ptr != nullptr);
    void* expected = nullptr;
    // We're using the "strong" version because in the next step we wait on the
    // expected value. If that value would be `nullptr` (which might happen in
    // the compare_exchange_weak) then we might block forever!
    while (!atomic.compare_exchange_strong(expected, ptr, std::memory_order_release,
                                           std::memory_order_relaxed)) {
      atomic.wait(expected, std::memory_order_relaxed);
      expected = nullptr;
    }
    // Consumer may be waiting so notify it. Side-effect of this is that other
    // produces will also be notified but it shouldn't matter.
    // TODO: maybe create a separate condition_variable for the consumer?
    // TODO: maybe allow for passing stop_token to recv()? (see
    // condition_variable_any::wait)
    atomic.notify_all();
  }

  void* try_send(void* ptr) {
    assert(ptr != nullptr);
    void* expected = nullptr;
    bool success = atomic.compare_exchange_strong(expected, ptr, std::memory_order_release,
                                                  std::memory_order_relaxed);
    if (success) {
      atomic.notify_all();
      return nullptr;
    }
    return ptr;
  }

  // Will never block.
  void send_force(void* ptr) {
    assert(ptr != nullptr);
    atomic.store(ptr, std::memory_order_release);
    atomic.notify_all();
  }

  void* peek() { return atomic.load(std::memory_order_acquire); }

  // May block.
  void* recv() {
    void* ptr = atomic.exchange(nullptr, std::memory_order_acquire);
    while (ptr == nullptr) {
      atomic.wait(nullptr, std::memory_order_relaxed);
      ptr = atomic.exchange(nullptr, std::memory_order_acquire);
    }
    atomic.notify_one();
    return ptr;
  }

  template <typename T>
  void send(std::unique_ptr<T> ptr) {
    send(ptr.release());
  }

  template <typename T>
  void send_force(std::unique_ptr<T> ptr) {
    send_force(ptr.release());
  }

  template <typename T>
  std::unique_ptr<T> try_send(std::unique_ptr<T> ptr) {
    return std::unique_ptr<T>(static_cast<T*>(try_send(ptr.release())));
  }

  template <typename T>
  std::unique_ptr<T> recv() {
    return std::unique_ptr<T>(static_cast<T*>(recv()));
  }

  template <typename T>
  T* peek() {
    return static_cast<T*>(peek());
  }
};

}  // namespace automat