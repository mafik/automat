#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include <sys/epoll.h>

#include <functional>
#include <mutex>
#include <stop_token>
#include <vector>

#include "fd.hpp"
#include "status.hpp"
#include "str.hpp"

// C++ wrappers around the Linux epoll facility.
namespace automat::mux {

struct Epoll {
  // Base class for objects that would like to receive epoll updates.
  struct Listener {
    // File descriptor monitored by this Listener.
    FD fd;

    // Whether this Listener is interested in NotifyRead.
    bool notify_read = true;

    // Whether this Listener is interested for NotifyWrite.
    bool notify_write = false;

    Listener() = default;
    Listener(FD fd) : fd(std::move(fd)) {}

    virtual ~Listener() = default;

    // Method called whenever a registered file descriptor becomes ready for
    // reading.
    virtual void NotifyRead(Status&) = 0;

    // Method called whenever a registered file descriptor becomes ready for
    // writing.
    virtual void NotifyWrite(Status&) {};

    virtual StrView Name() const = 0;

    // Less-than operator for use in std::set.
    bool operator<(const Listener& other) const { return fd < other.fd; }
  };

  Epoll() = default;
  Epoll(const Epoll&) = delete;
  Epoll& operator=(const Epoll&) = delete;

  void Init(Status&);

  // Add a new listener to this epoll instance.
  void Add(Listener*, Status&);

  // Change the listening flags of this listener. This can be called whenever a
  // Listener becomes (or stops being) interested in some type of update.
  void Mod(Listener*, Status&);

  // Remove the specified file descriptor from this epoll instance.
  void Del(Listener*, Status&);

  // Poll events until an error is returned, all listeners drop, or stop is
  // requested.
  void Loop(Status&, std::stop_token = {});

  // Runs fn on the Loop thread; callable from any thread.
  void Post(std::move_only_function<void()> fn);

 private:
  struct Wakeup : Listener {
    Epoll* epoll = nullptr;
    void NotifyRead(Status&) override;
    StrView Name() const override { return "EpollWakeup"sv; }
  };

  void Wake();

  // File descriptor of the epoll instance.
  int fd = 0;

  // Number of active Listeners.
  int listener_count = 0;

  static constexpr int kMaxEpollEvents = 10;
  epoll_event events[kMaxEpollEvents];
  int events_count = 0;

  Wakeup wakeup;
  std::mutex post_mutex;
  std::vector<std::move_only_function<void()>> posted;
};

}  // namespace automat::mux
