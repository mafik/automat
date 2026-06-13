#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include "fd.hh"
#include "status.hh"
#include "str.hh"

// C++ wrappers around the Linux epoll facility.
namespace automat::epoll {

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

// File descriptor of the epoll instance.
extern thread_local int fd;

// Number of active Listeners.
extern thread_local int listener_count;

void Init();

// Add a new listener to this epoll instance.
void Add(Listener*, Status&);

// Change the listening flags of this listener. This can be called whenever a
// Listener becomes (or stops being) interested in some type of update.
void Mod(Listener*, Status&);

// Remove the specified file descriptor from this epoll instance.
void Del(Listener*, Status&);

// Poll events until an error is returned or all listeners drop.
void Loop(Status&);

}  // namespace automat::epoll
