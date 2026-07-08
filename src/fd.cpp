// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
#include "fd.hpp"

#if defined(_WIN32)
#include <winsock2.h>
#pragma comment(lib, "ws2_32")
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "status.hpp"

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

#if defined(_WIN32)

// On Windows an FD holds a WinSock SOCKET. Kernel handles are guaranteed to fit
// in the lower 32 bits, so the int storage is safe.

void FD::SetNonBlocking(Status& status) {
  u_long nonblocking = 1;
  if (ioctlsocket((SOCKET)(intptr_t)fd, FIONBIO, &nonblocking) != 0) {
    AppendErrorMessage(status) += "ioctlsocket(FIONBIO) failed";
  }
}

void FD::Close() {
  if (fd >= 0) {
    closesocket((SOCKET)(intptr_t)fd);
    fd = -1;
  }
}

#else

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

#endif

bool FD::Opened() const { return fd >= 0; }

}  // namespace automat
