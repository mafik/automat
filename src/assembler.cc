// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#include "assembler.hh"

#include <llvm/MC/MCCodeEmitter.h>
#include <llvm/MC/MCInstBuilder.h>
#include <llvm/MC/MCInstPrinter.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/lib/Target/X86/X86Subtarget.h>
#include <sys/mman.h>  // For mmap related functions and constants

#include "status.hh"

#pragma maf add link argument "-lz"
#pragma maf add link argument "-lzstd"

using namespace llvm;
using namespace std;
using namespace maf;

namespace automat {

const Triple Assembler::kNativeTriple = Triple(kTripleStr);

std::optional<Assembler> assembler;

Assembler::Assembler(Status& status) {
  LLVMInitializeX86TargetInfo();
  LLVMInitializeX86Target();
  LLVMInitializeX86TargetMC();
  LLVMInitializeX86AsmPrinter();

  string err;
  target = TargetRegistry::lookupTarget(kTripleStr, err);
  if (!target) {
    AppendErrorMessage(status) += err;
    return;
  }

  TargetOptions target_options;
  target_machine = std::unique_ptr<TargetMachine>(
      target->createTargetMachine(kTripleStr, "generic", "", target_options, nullopt));
  mc_asm_info = target_machine->getMCAsmInfo();
  mc_instr_info = target_machine->getMCInstrInfo();
  mc_reg_info = target_machine->getMCRegisterInfo();
  mc_subtarget_info = target_machine->getMCSubtargetInfo();
  mc_context.emplace(kNativeTriple, mc_asm_info, mc_reg_info, mc_subtarget_info);

  mc_code_emitter.reset(target->createMCCodeEmitter(*mc_instr_info, *mc_context));
  mc_inst_printer.reset(target->createMCInstPrinter(kNativeTriple, 1 /*Intel*/, *mc_asm_info,
                                                    *mc_instr_info, *mc_reg_info));

  machine_code.reset((char*)mmap((void*)0x10000, kMachineCodeSize, PROT_READ | PROT_EXEC,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
}

void DeleteWithMunmap::operator()(void* ptr) const { munmap(ptr, kMachineCodeSize); }

void Assembler::UpdateMachineCode() {
  mprotect(machine_code.get(), kMachineCodeSize, PROT_READ | PROT_WRITE);

  memset(machine_code.get(), 0x90, kMachineCodeSize);

  size_t machine_code_offset = 0;

  SmallVector<char, 128> epilogue_prologue;
  SmallVector<MCFixup, 4> epilogue_prologue_fixups;

  // # EPILOGUE:
  // TODO: Save any registers which are marked as "persistent"
  // Restore callee-saved registers
  mc_code_emitter->encodeInstruction(MCInstBuilder(X86::POP64r).addReg(X86::R15), epilogue_prologue,
                                     epilogue_prologue_fixups, *mc_subtarget_info);
  mc_code_emitter->encodeInstruction(MCInstBuilder(X86::POP64r).addReg(X86::R14), epilogue_prologue,
                                     epilogue_prologue_fixups, *mc_subtarget_info);
  mc_code_emitter->encodeInstruction(MCInstBuilder(X86::POP64r).addReg(X86::R13), epilogue_prologue,
                                     epilogue_prologue_fixups, *mc_subtarget_info);
  mc_code_emitter->encodeInstruction(MCInstBuilder(X86::POP64r).addReg(X86::R12), epilogue_prologue,
                                     epilogue_prologue_fixups, *mc_subtarget_info);
  mc_code_emitter->encodeInstruction(MCInstBuilder(X86::POP64r).addReg(X86::RBP), epilogue_prologue,
                                     epilogue_prologue_fixups, *mc_subtarget_info);
  mc_code_emitter->encodeInstruction(MCInstBuilder(X86::POP64r).addReg(X86::RBX), epilogue_prologue,
                                     epilogue_prologue_fixups, *mc_subtarget_info);

  // Return
  mc_code_emitter->encodeInstruction(MCInstBuilder(X86::RET32), epilogue_prologue,
                                     epilogue_prologue_fixups, *mc_subtarget_info);

  auto epilogue_size = epilogue_prologue.size();

  // # PROLOGUE: (goes right after the epilogue)
  // Save callee-saved registers:
  mc_code_emitter->encodeInstruction(MCInstBuilder(X86::PUSH64r).addReg(X86::RBX),
                                     epilogue_prologue, epilogue_prologue_fixups,
                                     *mc_subtarget_info);
  mc_code_emitter->encodeInstruction(MCInstBuilder(X86::PUSH64r).addReg(X86::RBP),
                                     epilogue_prologue, epilogue_prologue_fixups,
                                     *mc_subtarget_info);
  mc_code_emitter->encodeInstruction(MCInstBuilder(X86::PUSH64r).addReg(X86::R12),
                                     epilogue_prologue, epilogue_prologue_fixups,
                                     *mc_subtarget_info);
  mc_code_emitter->encodeInstruction(MCInstBuilder(X86::PUSH64r).addReg(X86::R13),
                                     epilogue_prologue, epilogue_prologue_fixups,
                                     *mc_subtarget_info);
  mc_code_emitter->encodeInstruction(MCInstBuilder(X86::PUSH64r).addReg(X86::R14),
                                     epilogue_prologue, epilogue_prologue_fixups,
                                     *mc_subtarget_info);
  mc_code_emitter->encodeInstruction(MCInstBuilder(X86::PUSH64r).addReg(X86::R15),
                                     epilogue_prologue, epilogue_prologue_fixups,
                                     *mc_subtarget_info);

  // TODO: Load any saved registers from our state buffer

  // Jump to the first instruction (RDI)
  mc_code_emitter->encodeInstruction(MCInstBuilder(X86::JMP64r).addReg(X86::RDI), epilogue_prologue,
                                     epilogue_prologue_fixups, *mc_subtarget_info);

  auto prologue_size = epilogue_prologue.size() - epilogue_size;

  prologue_fn = reinterpret_cast<void (*)(void*)>(reinterpret_cast<intptr_t>(machine_code.get()) +
                                                  kMachineCodeSize - prologue_size);

  // Copy epilogue/prologue at the end of machine_code.
  void* epilogue_prologue_dest = reinterpret_cast<void*>(
      reinterpret_cast<intptr_t>(machine_code.get()) + kMachineCodeSize - epilogue_prologue.size());
  memcpy(epilogue_prologue_dest, epilogue_prologue.data(), epilogue_size + prologue_size);
  int64_t epilogue_prologue_addr = reinterpret_cast<int64_t>(epilogue_prologue_dest);

  // Go over all instructions. For each one:
  // - Emit machine code at the first available position in machine_code.
  // - TODO: Follow up with all the subsequent instructions (basic block).
  // - TODO: If the last instruction wasn't an unconditional jump, emit a jump to EPILOGUE.
  for (auto inst : instructions) {
    SmallVector<char, 128> code_bytes;
    SmallVector<MCFixup, 4> fixups;
    mc_code_emitter->encodeInstruction(inst->mc_inst, code_bytes, fixups, *mc_subtarget_info);

    if (!fixups.empty()) {
      ERROR << "Fixups not supported!";
    }
    fixups.clear();

    // Temprorarily emit JMP to epilogue unconditionally.
    mc_code_emitter->encodeInstruction(MCInstBuilder(X86::JMP_4).addImm(0), code_bytes, fixups,
                                       *mc_subtarget_info);

    int jmp_base = machine_code_offset + code_bytes.size();
    int jmp_target = kMachineCodeSize - epilogue_prologue.size();
    int jmp_pcrel = jmp_target - jmp_base;
    code_bytes.pop_back();
    code_bytes.pop_back();
    code_bytes.pop_back();
    code_bytes.pop_back();
    code_bytes.push_back(jmp_pcrel & 0xFF);
    code_bytes.push_back((jmp_pcrel >> 8) & 0xFF);
    code_bytes.push_back((jmp_pcrel >> 16) & 0xFF);
    code_bytes.push_back((jmp_pcrel >> 24) & 0xFF);

    void* dest = reinterpret_cast<void*>(reinterpret_cast<intptr_t>(machine_code.get()) +
                                         machine_code_offset);
    inst->address = dest;
    memcpy(dest, code_bytes.data(), code_bytes.size());
    machine_code_offset += code_bytes.size();
  }

  // string machine_code_str;
  // for (int i = 0; i < machine_code_offset; i++) {
  //   machine_code_str += f("%02x ", machine_code.get()[i]);
  //   if (i % 16 == 15) {
  //     machine_code_str += "\n";
  //   }
  // }
  // LOG << machine_code_str;

  // string epilogue_prologue_str;
  // for (int i = 0; i < epilogue_prologue.size(); i++) {
  //   epilogue_prologue_str += f("%02x ", epilogue_prologue[i]);
  //   if (i % 16 == 15) {
  //     epilogue_prologue_str += "\n";
  //   }
  // }
  // LOG << epilogue_prologue_str;

  mprotect(machine_code.get(), kMachineCodeSize, PROT_READ | PROT_EXEC);
}

void Assembler::RunMachineCode(library::Instruction* entry_point) {
  LOG << "Running machine code at " << f("%p", entry_point->address) << "...";
  prologue_fn(entry_point->address);
  LOG << "Done!";
}
}  // namespace automat
