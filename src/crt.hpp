#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include <functional>
#include <thread>

#include "status.hpp"
#include "unique_ptr.hpp"

// Provides access to host's libc (even when statically linked).

namespace automat::crt {

void Init(Status&);

void* OpenLibrary(const char* name, int mode);
void* FindSymbol(void* library, const char* name);
int CloseLibrary(void* library);
const char* LibraryError();

#if defined(__linux__)
void Free(void* pointer);
using DeleteWithFree = DeleteWith<Free>;
#endif

// ChimeraThread is a mix of static (cosmopolitan) & host (glibc) thread.
// It maintains the TLS buffers for both C libraries and can interact with both of them without any
// mode switching.

#if defined(__COSMOPOLITAN__)
class ChimeraThread {
 public:
  ChimeraThread() = default;
  explicit ChimeraThread(std::move_only_function<void()> body);
  ChimeraThread(ChimeraThread&& other) noexcept;
  ChimeraThread& operator=(ChimeraThread&& other) noexcept;
  ~ChimeraThread();

  bool joinable() const { return handle != 0; }
  void join();
  void detach();

 private:
  unsigned long handle = 0;
};
#else
using ChimeraThread = std::jthread;
#endif

inline ChimeraThread StartChimeraThread(std::move_only_function<void()> body) {
  return ChimeraThread(std::move(body));
}

}  // namespace automat::crt
