// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <llvm/MC/MCCodeEmitter.h>
#include <llvm/MC/MCContext.h>
#include <llvm/MC/MCInstPrinter.h>
#include <llvm/Target/TargetMachine.h>

namespace automat {

struct LLVM_Assembler {
  const llvm::Target* target;
  std::unique_ptr<llvm::TargetMachine> target_machine;
  const llvm::MCAsmInfo* mc_asm_info;
  const llvm::MCInstrInfo* mc_instr_info;
  const llvm::MCRegisterInfo* mc_reg_info;
  const llvm::MCSubtargetInfo* mc_subtarget_info;
  std::optional<llvm::MCContext> mc_context;
  std::unique_ptr<llvm::MCCodeEmitter> mc_code_emitter;
  std::unique_ptr<llvm::MCInstPrinter> mc_inst_printer;

  static LLVM_Assembler& Get();
};

}  // namespace automat