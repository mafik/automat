#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
// Warning: coded with a stochastic parrot

#include <chrono>
#include <functional>
#include <mutex>
#include <stop_token>
#include <vector>

#include "fd.hpp"
#include "status.hpp"
#include "str.hpp"

// WSAPoll-based twin of the Linux epoll reactor (mux_epoll.hpp & mux_timer.hpp).
// Windows can only multiplex sockets this way; other waitable objects reach the
// loop thread through Post() or WatchProcess().
namespace automat::mux {

struct Timer;

struct Epoll {
  // Base class for objects that would like to receive socket updates.
  struct Listener {
    // Holds a WinSock SOCKET. See fd.cpp for why an int is enough.
    FD fd;

    // Whether this Listener is interested in NotifyRead.
    bool notify_read = true;

    // Whether this Listener is interested for NotifyWrite.
    bool notify_write = false;

    Listener() = default;
    Listener(FD fd) : fd(std::move(fd)) {}

    virtual ~Listener() = default;

    // Method called whenever a registered socket becomes ready for reading.
    virtual void NotifyRead(Status&) = 0;

    // Method called whenever a registered socket becomes ready for writing.
    virtual void NotifyWrite(Status&) {};

    virtual StrView Name() const = 0;

    // Less-than operator for use in std::set.
    bool operator<(const Listener& other) const { return fd < other.fd; }
  };

  Epoll() = default;
  Epoll(const Epoll&) = delete;
  Epoll& operator=(const Epoll&) = delete;

  void Init(Status&);

  // Add a new listener to this poll instance.
  void Add(Listener*, Status&);

  // Change the listening flags of this listener. This can be called whenever a
  // Listener becomes (or stops being) interested in some type of update.
  void Mod(Listener*, Status&);

  // Remove the specified listener from this poll instance.
  void Del(Listener*, Status&);

  // Poll events until an error is returned, all listeners drop, or stop is
  // requested.
  void Loop(Status&, bool stop_when_empty = false, std::stop_token = {});

  // Runs fn on the Loop thread; callable from any thread.
  void Post(std::move_only_function<void()> fn);

 private:
  friend struct Timer;

  void Wake();

  bool initialized = false;

  // Self-connected loopback UDP socket; a one byte send interrupts WSAPoll.
  FD wakeup;

  std::mutex mutex;
  std::vector<Listener*> listeners;
  // Snapshot served by the current WSAPoll round. Del() nulls its entries so
  // in-flight events of removed listeners are skipped, like on Linux.
  std::vector<Listener*> batch;

  std::mutex timer_mutex;
  std::vector<Timer*> timers;
  std::vector<Timer*> due;  // ~Timer() nulls entries mid-dispatch

  std::mutex post_mutex;
  std::vector<std::move_only_function<void()>> posted;
};

// Process-wide shared reactor.
extern Epoll epoll;

// Starts a dedicated thread running epoll.Loop().
void Init(Status&);
void Stop();

// Watches a child process for its exit status. On Windows `wait_status` is the
// raw process exit code rather than a waitpid-style bitfield.
void WatchProcess(int pid, std::function<void(int wait_status)> on_exit, Status&);

// Deadline-based twin of the Linux timerfd Timer, driven by the poll timeout.
struct Timer {
  Epoll& epoll;
  Status status;
  std::function<void()> handler;

  Timer(Epoll& epoll);
  ~Timer();

  // Setting `initial_s` to zero disarms the timer.
  void Arm(double initial_s, double interval_s = 0);
  void Disarm();

 private:
  friend struct Epoll;
  bool armed = false;
  std::chrono::steady_clock::time_point deadline;
  double interval = 0;  // seconds; 0 means one-shot
};

}  // namespace automat::mux
