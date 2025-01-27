// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <llvm/MC/MCCodeEmitter.h>
#include <llvm/MC/MCContext.h>
#include <llvm/MC/MCInstPrinter.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Target/TargetMachine.h>

#include "library_instruction.hh"
#include "object.hh"
#include "status.hh"

namespace automat {

constexpr size_t kMachineCodeSize = 1024 * 4;

struct DeleteWithMunmap {
  void operator()(void* ptr) const;
};

struct Regs;

// Combines functions of Assembler and Thread.
// Assembler part takes care of emitting machine code to an executable memory region.
// Thread part maintains register state across executions.
struct Assembler : LiveObject {
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

  std::unique_ptr<Regs> regs;

  vector<library::Instruction*> instructions;

  std::shared_ptr<Object> Clone() const override;

  void (*prologue_fn)(void*) = nullptr;

  Assembler(maf::Status&);
  ~Assembler();

  void UpdateMachineCode();

  void RunMachineCode(library::Instruction* entry_point);

  struct Widget : FallbackWidget {
    Widget(std::weak_ptr<Object> object) : FallbackWidget() { this->object = object; }
    float Width() const override { return 6_cm; }
    std::string Text() const override;
  };

  std::shared_ptr<gui::Widget> MakeWidget() override { return std::make_shared<Widget>(WeakPtr()); }
};

}  // namespace automat
