#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include "status.hh"

namespace automat {

// Wrapper around a file descriptor.
struct FD {
  int fd;

  FD();
  FD(int fd);
  FD(const FD&) = delete;
  FD(FD&& other);
  ~FD();

  operator int() const { return fd; }

  FD& operator=(const FD&) = delete;
  FD& operator=(FD&& other);

  void Close();

  bool Opened() const;

  void SetNonBlocking(Status&);
};

}  // namespace automat
