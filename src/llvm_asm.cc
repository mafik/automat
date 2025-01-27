// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#include "llvm_asm.hh"

#include <llvm/MC/MCCodeEmitter.h>
#include <llvm/MC/MCInstBuilder.h>
#include <llvm/MC/MCInstPrinter.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/lib/Target/X86/X86Subtarget.h>

#include <memory>

using namespace std;
using namespace llvm;

namespace automat {

constexpr static char kTripleStr[] = "x86_64-pc-linux-gnu";
const Triple kTriple = Triple(kTripleStr);

LLVM_Assembler& LLVM_Assembler::Get() {
  static unique_ptr<LLVM_Assembler> x86_64_assembler = []() {
    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86Target();
    LLVMInitializeX86TargetMC();
    LLVMInitializeX86AsmPrinter();

    auto a = make_unique<LLVM_Assembler>();

    string err;
    a->target = TargetRegistry::lookupTarget(kTripleStr, err);
    assert(a->target);

    TargetOptions target_options;
    a->target_machine = std::unique_ptr<TargetMachine>(
        a->target->createTargetMachine(kTripleStr, "generic", "", target_options, nullopt));
    a->mc_asm_info = a->target_machine->getMCAsmInfo();
    a->mc_instr_info = a->target_machine->getMCInstrInfo();
    a->mc_reg_info = a->target_machine->getMCRegisterInfo();
    a->mc_subtarget_info = a->target_machine->getMCSubtargetInfo();
    a->mc_context.emplace(kTriple, a->mc_asm_info, a->mc_reg_info, a->mc_subtarget_info);

    a->mc_code_emitter.reset(a->target->createMCCodeEmitter(*a->mc_instr_info, *a->mc_context));
    a->mc_inst_printer.reset(a->target->createMCInstPrinter(kTriple, 1 /*Intel*/, *a->mc_asm_info,
                                                            *a->mc_instr_info, *a->mc_reg_info));

    return a;
  }();
  return *x86_64_assembler;
}

}  // namespace automat