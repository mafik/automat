#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include <cstdint>

#include "optional.hpp"
#include "source_location.hpp"

namespace automat {

struct LowLevelError {
  enum class Type {
    EXECUTED_UNKNOWN_INSTRUCTION,  // SIGILL
    READ_PROTECTED_MEMORY,         // SIGSEGV
    WROTE_PROTECTED_MEMORY,        // SIGSEGV
    EXECUTED_PROTECTED_MEMORY,     // SIGSEGV
    READ_UNMAPPED_MEMORY,          // SIGSEGV
    WROTE_UNMAPPED_MEMORY,         // SIGSEGV
    EXECUTED_UNMAPPED_MEMORY,      // SIGSEGV
    ACCESSED_UNALIGNED_MEMORY,     // SIGBUS
    ARITHMETIC_ERROR,              // SIGFPE
    STACK_OVERFLOW,                // SIGSEGV
  } type;
  intptr_t instruction_pointer;

  Optional<SourceLocation> FindSourceLocation() const;
};

namespace error_recovery {

// Per-thread stack for signal delivery, required for stack overflow recovery.
struct SignalStack {
  char stack[32 * 1024];
  SignalStack();
  ~SignalStack();
  SignalStack(const SignalStack&) = delete;
};

void Init();  // Sets up signal handlers (process-wide)
void Stop();  // Cleans up its signal handlers

}  // namespace error_recovery

}  // namespace automat