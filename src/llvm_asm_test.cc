// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include <span>
#include <vector>

#include "concurrentqueue.hh"
#include "gtest.hh"

struct Instruction;
struct Argument;

// Represents a section of executable machine code.
struct MappedCode {
  // Enters code at a given `start_addr`. Returns the address right after the exit point.
  using EntryFunc = uint64_t (*)(uint64_t start_addr);

  // Range of memory where the code is mapped at.
  std::span<char> mem;

  // Address at which the machine code can be entered. See `EntryFunc` description.
  EntryFunc Enter;

  // Describes a section of code
  struct MapEntry {
    // Range of memory described by this entry.
    std::span<char> mem;

    // The Instruction that this section of code belongs to.
    Instruction* instruction;

    // Only populated for exit points - indicates which argument the exit point corresponds to.
    Argument* argument;
  };

  std::vector<MapEntry> map;
};

// Controls the execution of assembly code. Thread-safe.
//
// Internally uses two threads - a worker thread, which executes the actual assembly and control
// thread which functions as a debugger.
struct CodeController {
  struct Task {};

  // Holds commands from the main thread.
  moodycamel::ConcurrentQueue<Task> control_commands;

  std::jthread control_thread;

  CodeController() {}

  ~CodeController() {}
};

TEST(LLVMAsm, Empty) { EXPECT_TRUE(true); }
