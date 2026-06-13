// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
#include "fd.hh"

#include <fcntl.h>
#include <unistd.h>

#include "status.hh"

namespace automat {

FD::FD() : fd(-1) {}
FD::FD(int fd) : fd(fd) {}
FD::FD(FD&& other) : fd(other.fd) { other.fd = -1; }
FD::~FD() { Close(); }

FD& FD::operator=(FD&& other) {
  Close();
  fd = other.fd;
  other.fd = -1;
  return *this;
}

void FD::SetNonBlocking(Status& status) {
  int flags = fcntl(fd, F_GETFL);
  if (flags < 0) {
    AppendErrorMessage(status) += "fcntl(F_GETFL) failed";
    return;
  }

  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    AppendErrorMessage(status) += "fcntl(F_SETFL) failed";
    return;
  }
}

void FD::Close() {
  if (fd >= 0) {
    close(fd);
    fd = -1;
  }
}

bool FD::Opened() const { return fd >= 0; }

}  // namespace automat
