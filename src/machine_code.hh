// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <llvm/MC/MCInstBuilder.h>

#include <vector>

#include "status.hh"

// Namespace for Machine Code related functions
namespace automat::mc {

using Inst = llvm::MCInst;
using InstBuilder = llvm::MCInstBuilder;

// Represents a single instruction within a larger program.
struct ProgramInst {
  std::shared_ptr<const Inst> inst;
  int next;  // index of the next instruction within the program
  int jump;  // index of the jump target within the program
};

using Program = std::vector<ProgramInst>;

struct Regs {
  uint64_t RAX = 0;
  uint64_t RBX = 0;
  uint64_t RCX = 0;
  uint64_t RDX = 0;
  uint64_t RBP = 0;
  uint64_t RSI = 0;
  uint64_t RDI = 0;
  uint64_t R8 = 0;
  uint64_t R9 = 0;
  uint64_t R10 = 0;
  uint64_t R11 = 0;
  uint64_t R12 = 0;
  uint64_t R13 = 0;
  uint64_t R14 = 0;
  uint64_t R15 = 0;

  uint64_t& operator[](int index) { return reinterpret_cast<uint64_t*>(this)[index]; }
};

enum class CodeType : uint8_t {
  InstructionBody,
  Next,
  Jump,
};

struct CodePoint {
  std::weak_ptr<const Inst>* instruction;  // valid until next code update
  CodeType code_type;
};

using ExitCallback = std::function<void(CodePoint)>;

// Controls the execution of machine code.
//
// Thread-safe - methods can be called from many threads.
struct Controller {
  virtual ~Controller() = default;

  // Convert the given instructions into machine code, hot-reloading if necessary. Thread-safe.
  //
  // Program must be sorted using std::owner_less.
  virtual void UpdateCode(Program&& program, maf::Status&) = 0;

  virtual void Execute(std::weak_ptr<Inst> instr, maf::Status&) = 0;

  struct State {
    // Instruction which is about to be executed.
    std::weak_ptr<const Inst> current_instruction;

    // State of registers prior to the current instruction.
    Regs regs;
  };

  virtual void GetState(State&, maf::Status&) = 0;

  // ExitCallback is going to be called when the machine code exits or crashes.
  static std::unique_ptr<Controller> Make(ExitCallback&& exit_callback);

 protected:
  Controller(ExitCallback&& exit_callback) : exit_callback(exit_callback) {}

  ExitCallback exit_callback;
};

}  // namespace automat::mc