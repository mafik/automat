// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#include "library_assembler.hh"

#include <llvm/MC/MCCodeEmitter.h>
#include <llvm/MC/MCInstBuilder.h>
#include <llvm/MC/MCInstPrinter.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/lib/Target/X86/X86Subtarget.h>
#include <signal.h>
#include <sys/mman.h>  // For mmap related functions and constants
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/wait.h>

#include "status.hh"

#pragma maf add link argument "-lz"
#pragma maf add link argument "-lzstd"

using namespace llvm;
using namespace std;
using namespace maf;

namespace automat {

constexpr static char kTripleStr[] = "x86_64-pc-linux-gnu";

const Triple Assembler::kNativeTriple = Triple(kTripleStr);

void AssemblerSignalHandler(int sig, siginfo_t* si, struct ucontext_t* context) {
  // In Automat this handler will actually call Automat code to see what to do.
  // That code may block the current thread.
  // Response may be either to restart from scratch (longjmp), retry (return) or exit the thread
  // (longjmp).

  printf("\n*** Caught signal %d (%s) ***\n", sig, strsignal(sig));
  printf("Signal originated at address: %p\n", si->si_addr);
  printf("si_addr_lsb: %d\n", si->si_addr_lsb);
  __builtin_dump_struct(context, &printf);
  printf("gregs: ");
  auto& gregs = context->uc_mcontext.gregs;
  for (int i = 0; i < sizeof(gregs) / sizeof(gregs[0]); ++i) {
    printf("%lx ", gregs[i]);
  }
  printf("\n");

  // Print additional signal info
  printf("Signal code: %d\n", si->si_code);
  printf("Faulting process ID: %d\n", si->si_pid);
  printf("User ID of sender: %d\n", si->si_uid);

  exit(1);
}

static bool SetupSignalHandler() {
  struct sigaction sa;
  memset(&sa, 0, sizeof(struct sigaction));
  // sigemptyset(&sa.sa_mask);
  sigfillset(&sa.sa_mask);
  sa.sa_sigaction = (void (*)(int, siginfo_t*, void*))AssemblerSignalHandler;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;

  if (sigaction(SIGSEGV, &sa, NULL) == -1) {
    perror("Failed to set SIGSEGV handler");
    return false;
  }
  if (sigaction(SIGILL, &sa, NULL) == -1) {
    perror("Failed to set SIGILL handler");
    return false;
  }
  if (sigaction(SIGBUS, &sa, NULL) == -1) {
    perror("Failed to set SIGBUS handler");
    return false;
  }
  return true;
}

// Note: We're not preserving RSP!
// CB(RSP)
#define REGS(CB) \
  CB(RBX)        \
  CB(RCX)        \
  CB(RDX)        \
  CB(RBP)        \
  CB(RSI)        \
  CB(RDI)        \
  CB(R8)         \
  CB(R9)         \
  CB(R10)        \
  CB(R11)        \
  CB(R12)        \
  CB(R13)        \
  CB(R14)        \
  CB(R15)

#define ALL_REGS(CB) \
  CB(RAX)            \
  REGS(CB)

struct Regs {
#define CB(reg) uint64_t reg = 0;
  ALL_REGS(CB);
  // CB(original_RSP);
#undef CB
};

Assembler::Assembler(Status& status) {
  LLVMInitializeX86TargetInfo();
  LLVMInitializeX86Target();
  LLVMInitializeX86TargetMC();
  LLVMInitializeX86AsmPrinter();

  static bool signal_handler_initialized =
      SetupSignalHandler();  // unused, ensures that initialization happens once

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

  regs = std::make_unique<Regs>();
}

Assembler::~Assembler() {}

std::shared_ptr<Object> Assembler::Clone() const {
  Status status;
  auto obj = std::make_shared<Assembler>(status);
  if (OK(status)) {
    return obj;
  }
  return nullptr;
}

void DeleteWithMunmap::operator()(void* ptr) const { munmap(ptr, kMachineCodeSize); }

void Assembler::UpdateMachineCode() {
  mprotect(machine_code.get(), kMachineCodeSize, PROT_READ | PROT_WRITE);

  memset(machine_code.get(), 0x90, kMachineCodeSize);

  size_t machine_code_offset = 0;

  SmallVector<char, 256> epilogue_prologue;
  SmallVector<MCFixup, 4> epilogue_prologue_fixups;
  int64_t regs_addr = reinterpret_cast<int64_t>(regs.get());

  auto MOVmRAX = [&](int64_t addr) {
    mc_code_emitter->encodeInstruction(MCInstBuilder(X86::MOV64o64a).addImm(addr).addReg(0),
                                       epilogue_prologue, epilogue_prologue_fixups,
                                       *mc_subtarget_info);
  };
  auto PUSHr = [&](unsigned reg) {
    mc_code_emitter->encodeInstruction(MCInstBuilder(X86::PUSH64r).addReg(reg), epilogue_prologue,
                                       epilogue_prologue_fixups, *mc_subtarget_info);
  };
  auto POPr = [&](unsigned reg) {
    mc_code_emitter->encodeInstruction(MCInstBuilder(X86::POP64r).addReg(reg), epilogue_prologue,
                                       epilogue_prologue_fixups, *mc_subtarget_info);
  };
  auto MOVri = [&](unsigned reg, uint64_t imm) {
    mc_code_emitter->encodeInstruction(MCInstBuilder(X86::MOV64ri).addReg(reg).addImm(imm),
                                       epilogue_prologue, epilogue_prologue_fixups,
                                       *mc_subtarget_info);
  };
  auto MOVmr = [&](unsigned addr_reg, unsigned addr_offset, unsigned reg) {
    mc_code_emitter->encodeInstruction(MCInstBuilder(X86::MOV64mr)
                                           .addReg(addr_reg)
                                           .addImm(1)
                                           .addReg(0)
                                           .addImm(addr_offset)
                                           .addReg(0)
                                           .addReg(reg),
                                       epilogue_prologue, epilogue_prologue_fixups,
                                       *mc_subtarget_info);
  };
  auto MOVrm = [&](unsigned reg, unsigned addr_reg, unsigned addr_offset) {
    mc_code_emitter->encodeInstruction(MCInstBuilder(X86::MOV64rm)
                                           .addReg(reg)
                                           .addReg(addr_reg)
                                           .addImm(1)
                                           .addReg(0)
                                           .addImm(addr_offset)
                                           .addReg(0),
                                       epilogue_prologue, epilogue_prologue_fixups,
                                       *mc_subtarget_info);
  };

  // # EPILOGUE:

  MOVmRAX(regs_addr);          // Store RAX at the start of Regs
  MOVri(X86::RAX, regs_addr);  // Put Regs address in RAX
#define SAVE(reg) MOVmr(X86::RAX, offsetof(Regs, reg), X86::reg);
  REGS(SAVE);  // Save all registers to Regs
#undef SAVE
  // MOVrm(X86::RSP, X86::RAX, offsetof(Regs, original_RSP));

  // Restore callee-saved registers
  POPr(X86::R15);
  POPr(X86::R14);
  POPr(X86::R13);
  POPr(X86::R12);
  POPr(X86::RBP);
  POPr(X86::RBX);

  // Return
  mc_code_emitter->encodeInstruction(MCInstBuilder(X86::RET32), epilogue_prologue,
                                     epilogue_prologue_fixups, *mc_subtarget_info);

  auto epilogue_size = epilogue_prologue.size();

  // # PROLOGUE: (goes right after the epilogue)
  // Save callee-saved registers:

  PUSHr(X86::RBX);
  PUSHr(X86::RBP);
  PUSHr(X86::R12);
  PUSHr(X86::R13);
  PUSHr(X86::R14);
  PUSHr(X86::R15);

  PUSHr(X86::RDI);  // Push the first argument (RDI) so that it can be used to "RET" into the right
                    // address

  MOVri(X86::RAX, regs_addr);
  // MOVmr(X86::RAX, offsetof(Regs, original_RSP), X86::RSP);
#define LOAD(reg) MOVrm(X86::reg, X86::RAX, offsetof(Regs, reg));
  REGS(LOAD);
  LOAD(RAX);  // load RAX last because it's used as a base for address
#undef LOAD

  // Jump to the first instruction (on top of the stack)
  mc_code_emitter->encodeInstruction(MCInstBuilder(X86::RET64), epilogue_prologue,
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

    auto inst_size = code_bytes.size();

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
    inst->size = inst_size;
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

std::string Assembler::Widget::Text() const {
  std::string s = "RAX = ";
  if (auto obj = object.lock()) {
    if (auto assembler = std::dynamic_pointer_cast<Assembler>(obj)) {
      s += f("%lx", assembler->regs->RAX);
    }
  }
  return s;
}

void Assembler::RunMachineCode(library::Instruction* entry_point) {
  int STACK_SIZE = 8 * 1024 * 1024;
  std::vector<char> stack;
  stack.resize(STACK_SIZE);
  pid_t v;
  struct ThreadArg {
    void (*prologue_fn)(void*);
    void* entry_point;
  };
  auto Thread = [](void* void_arg) {
    ThreadArg* arg = (ThreadArg*)void_arg;
    prctl(PR_SET_DUMPABLE, 1);

    arg->prologue_fn(arg->entry_point);
    return 0;
  };
  ThreadArg arg = {prologue_fn, entry_point->address};
  if (clone(Thread, (void*)(stack.data() + STACK_SIZE),
            CLONE_PARENT_SETTID | CLONE_SIGHAND | CLONE_FILES | CLONE_FS | CLONE_IO | CLONE_VM,
            &arg, &v) == -1) {
    perror("failed to spawn child task");
    return;
  }

  printf("t1_pid: %d\n", v);

  int ret = ptrace(PTRACE_SEIZE, v, 0, PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL);
  if (ret != 0) {
    perror("PTRACE_SEIZE failed");
    return;
  }

  LOG << "PTRACE_SEIZE done";

  // Wait for the child to exit
  waitpid(v, NULL, 0);

  LOG << "Child exited";
}
}  // namespace automat
