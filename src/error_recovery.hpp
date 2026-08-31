#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include <cstdint>
#include <utility>

#include "optional.hpp"
#include "source_location.hpp"

namespace automat {

struct LowLevelError {
  enum class Type {
    NONE,
    EXECUTED_UNKNOWN_INSTRUCTION,
    READ_PROTECTED_MEMORY,
    WROTE_PROTECTED_MEMORY,
    EXECUTED_PROTECTED_MEMORY,
    READ_UNMAPPED_MEMORY,
    WROTE_UNMAPPED_MEMORY,
    EXECUTED_UNMAPPED_MEMORY,
    ACCESSED_UNALIGNED_MEMORY,
    ARITHMETIC_ERROR,
    STACK_OVERFLOW,
  } type = Type::NONE;
  intptr_t instruction_pointer = 0;

  LowLevelError() = default;
  LowLevelError(Type type, intptr_t instruction_pointer)
      : type(type), instruction_pointer(instruction_pointer) {}
  LowLevelError(LowLevelError&& other)
      : type(other.type), instruction_pointer(other.instruction_pointer) {
    other.type = Type::NONE;
  }
  LowLevelError& operator=(LowLevelError&& other) {
    type = other.type;
    instruction_pointer = other.instruction_pointer;
    other.type = Type::NONE;
    return *this;
  }
  ~LowLevelError();

  Optional<SourceLocation> FindSourceLocation() const;
};

namespace error_recovery {

// Per-thread stack for signal delivery, required for stack overflow recovery.
struct SignalStack {
#if defined(__linux__)
  char stack[32 * 1024];
#endif
  SignalStack();
  ~SignalStack();
  SignalStack(const SignalStack&) = delete;
};

void Init();  // Sets up signal handlers (process-wide)
void Stop();  // Cleans up its signal handlers

}  // namespace error_recovery

}  // namespace automat

#define ERROR_RECOVERY_TRY                          \
  ::automat::LowLevelError caught_low_level_error;  \
  try

#define ERROR_RECOVERY_CATCH(name)                              \
  catch (::automat::LowLevelError& thrown_low_level_error) {    \
    caught_low_level_error = std::move(thrown_low_level_error); \
  }                                                             \
  if (::automat::LowLevelError& name = caught_low_level_error;  \
      name.type != ::automat::LowLevelError::Type::NONE)
