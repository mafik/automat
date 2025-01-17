// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <llvm/MC/MCCodeEmitter.h>
#include <llvm/MC/MCContext.h>
#include <llvm/MC/MCInstPrinter.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Target/TargetMachine.h>

#include "library_instruction.hh"
#include "status.hh"

namespace automat {

constexpr size_t kMachineCodeSize = 1024 * 4;

struct DeleteWithMunmap {
  void operator()(void* ptr) const;
};

struct Assembler {
  constexpr static char kTripleStr[] = "x86_64-pc-linux-gnu";
  const static llvm::Triple kNativeTriple;

  const llvm::Target* target;
  std::unique_ptr<llvm::TargetMachine> target_machine;
  const llvm::MCAsmInfo* mc_asm_info;
  const llvm::MCInstrInfo* mc_instr_info;
  const llvm::MCRegisterInfo* mc_reg_info;
  const llvm::MCSubtargetInfo* mc_subtarget_info;
  std::optional<llvm::MCContext> mc_context;
  std::unique_ptr<llvm::MCCodeEmitter> mc_code_emitter;
  std::unique_ptr<llvm::MCInstPrinter> mc_inst_printer;
  std::unique_ptr<char, DeleteWithMunmap> machine_code;

  vector<library::Instruction*> instructions;

  void (*prologue_fn)(void*) = nullptr;

  Assembler(maf::Status&);

  void UpdateMachineCode();

  void RunMachineCode(library::Instruction* entry_point);
};

extern std::optional<Assembler> assembler;

}  // namespace automat
