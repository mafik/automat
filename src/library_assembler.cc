// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#include "library_assembler.hh"

#include <llvm/MC/MCCodeEmitter.h>
#include <llvm/MC/MCInstBuilder.h>
#include <llvm/MC/MCInstPrinter.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/lib/Target/X86/X86Subtarget.h>

#if defined __linux__
#include <signal.h>
#include <sys/mman.h>  // For mmap related functions and constants
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#endif  // __linux__

#include "llvm_asm.hh"
#include "status.hh"

#if defined _WIN32
#pragma comment(lib, "ntdll.lib")
#endif  // __linux__

using namespace llvm;
using namespace std;
using namespace maf;

namespace automat::library {

#if defined __linux__
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

#else

// TODO: Implement signal handler on Windows
static bool SetupSignalHandler() { return true; }
#endif  // __linux__

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
  static bool signal_handler_initialized =
      SetupSignalHandler();  // unused, ensures that initialization happens once

#if defined __linux__
  machine_code.reset((char*)mmap((void*)0x10000, kMachineCodeSize, PROT_READ | PROT_EXEC,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
#else
  // TODO: Implement on Windows
  machine_code.reset(new char[kMachineCodeSize]);
#endif  // __linux__

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

void DeleteWithMunmap::operator()(void* ptr) const {
#if defined __linux__
  munmap(ptr, kMachineCodeSize);
#else
  // TODO: Implement on Windows
  delete[] (char*)ptr;
#endif  // __linux__
}

void Assembler::UpdateMachineCode() {
#if defined __linux__
  mprotect(machine_code.get(), kMachineCodeSize, PROT_READ | PROT_WRITE);
#else
  // TODO: Implement on Windows
#endif  // __linux__

  memset(machine_code.get(), 0x90, kMachineCodeSize);

  size_t machine_code_offset = 0;

  SmallVector<char, 256> epilogue_prologue;
  SmallVector<MCFixup, 4> epilogue_prologue_fixups;
  int64_t regs_addr = reinterpret_cast<int64_t>(regs.get());

  auto& llvm_asm = LLVM_Assembler::Get();
  auto& mc_code_emitter = llvm_asm.mc_code_emitter;
  auto& mc_subtarget_info = llvm_asm.mc_subtarget_info;

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

  // Store the 64-bit address of the exit point in
  POPr(X86::RAX);

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

  prologue_fn = reinterpret_cast<PrologueFn>(reinterpret_cast<intptr_t>(machine_code.get()) +
                                             kMachineCodeSize - prologue_size);

  // Copy epilogue/prologue at the end of machine_code.
  void* epilogue_prologue_dest = reinterpret_cast<void*>(
      reinterpret_cast<intptr_t>(machine_code.get()) + kMachineCodeSize - epilogue_prologue.size());
  memcpy(epilogue_prologue_dest, epilogue_prologue.data(), epilogue_size + prologue_size);
  int64_t epilogue_prologue_addr = reinterpret_cast<int64_t>(epilogue_prologue_dest);

  // Find all the x86 instructions
  auto here_ptr = here.lock();
  auto [begin, end] = here_ptr->incoming.equal_range(&assembler_arg);
  struct InstructionEntry {
    Location* loc;
    Instruction* inst;
    Connection* conn;
    Vec2 position;
  };
  Vec<InstructionEntry> instructions;
  for (auto it = begin; it != end; ++it) {
    auto& conn = *it;
    auto& inst_loc = conn->from;
    auto inst = inst_loc.As<Instruction>();
    instructions.push_back({&inst_loc, inst, conn, inst_loc.position});
  }

  for (auto& entry : instructions) {
    entry.inst->address = nullptr;
  }

  struct Fixup {
    size_t end_offset;  // place in machine_code where the fixup ends
    Instruction* target = nullptr;
    MCFixupKind kind = MCFixupKind::FK_PCRel_4;
  };

  Vec<Fixup> machine_code_fixups;

  auto EmitInstruction = [&](Location& loc, Instruction& inst) {
    SmallVector<char, 32> instruction_bytes;
    SmallVector<MCFixup, 4> instruction_fixups;
    mc_code_emitter->encodeInstruction(inst.mc_inst, instruction_bytes, instruction_fixups,
                                       *mc_subtarget_info);

    auto inst_size = instruction_bytes.size();

    void* dest = reinterpret_cast<void*>(reinterpret_cast<intptr_t>(machine_code.get()) +
                                         machine_code_offset);
    memcpy(dest, instruction_bytes.data(), instruction_bytes.size());
    machine_code_offset += instruction_bytes.size();

    if (instruction_fixups.size() > 1) {
      ERROR << "Instructions with more than one fixup not supported!";
    } else if (instruction_fixups.size() == 1) {
      auto& fixup = instruction_fixups[0];

      Instruction* jump_inst = nullptr;
      if (auto it = loc.outgoing.find(&jump_arg); it != loc.outgoing.end()) {
        jump_inst = (*it)->to.As<Instruction>();
      }
      // We assume that the fixup is at the end of the instruction.
      machine_code_fixups.push_back({machine_code_offset, jump_inst, fixup.getKind()});
    }
  };

  auto Fixup1 = [&](size_t fixup_end, size_t target_offset) {
    ssize_t pcrel = (ssize_t)(target_offset) - (ssize_t)(fixup_end);
    char* code = machine_code.get();
    code[fixup_end - 1] = (pcrel & 0xFF);
  };

  auto Fixup4 = [&](size_t fixup_end, size_t target_offset) {
    ssize_t pcrel = (ssize_t)(target_offset) - (ssize_t)(fixup_end);
    char* code = machine_code.get();
    code[fixup_end - 4] = (pcrel & 0xFF);
    code[fixup_end - 3] = ((pcrel >> 8) & 0xFF);
    code[fixup_end - 2] = ((pcrel >> 16) & 0xFF);
    code[fixup_end - 1] = ((pcrel >> 24) & 0xFF);
  };

  auto EmitJump = [&](size_t target_offset) {
    SmallVector<char, 32> instruction_bytes;
    SmallVector<MCFixup, 4> instruction_fixups;
    // Save the current RIP and jump to epilogue
    mc_code_emitter->encodeInstruction(MCInstBuilder(X86::JMP_4).addImm(0), instruction_bytes,
                                       instruction_fixups, *mc_subtarget_info);
    void* dest = reinterpret_cast<void*>(reinterpret_cast<intptr_t>(machine_code.get()) +
                                         machine_code_offset);
    memcpy(dest, instruction_bytes.data(), instruction_bytes.size());
    machine_code_offset += instruction_bytes.size();
    Fixup4(machine_code_offset, target_offset);
  };

  auto EmitExitPoint = [&]() {
    SmallVector<char, 32> instruction_bytes;
    SmallVector<MCFixup, 4> instruction_fixups;
    // Save the current RIP and jump to epilogue
    mc_code_emitter->encodeInstruction(MCInstBuilder(X86::CALL64pcrel32).addImm(0),
                                       instruction_bytes, instruction_fixups, *mc_subtarget_info);
    void* dest = reinterpret_cast<void*>(reinterpret_cast<intptr_t>(machine_code.get()) +
                                         machine_code_offset);
    memcpy(dest, instruction_bytes.data(), instruction_bytes.size());
    machine_code_offset += instruction_bytes.size();
    size_t jmp_target = kMachineCodeSize - epilogue_prologue.size();
    Fixup4(machine_code_offset, jmp_target);
  };

  for (auto& entry : instructions) {
    Location* loc = entry.loc;
    Instruction* inst = entry.inst;
    if (inst->address) {
      continue;
    }

    while (inst) {
      if (inst->address) {
        // current instruction was already emitted - jump to it instead
        EmitJump(reinterpret_cast<size_t>(inst->address) -
                 reinterpret_cast<size_t>(machine_code.get()));
        break;
      }
      inst->address = reinterpret_cast<void*>(reinterpret_cast<intptr_t>(machine_code.get()) +
                                              machine_code_offset);
      EmitInstruction(*loc, *inst);

      if (auto it = loc->outgoing.find(&next_arg); it != loc->outgoing.end()) {
        loc = &(*it)->to;
        inst = loc->As<Instruction>();
      } else {
        loc = nullptr;
        inst = nullptr;
      }
    }
    // TODO: don't emit the final exit point if the last instruction is a terminator
    EmitExitPoint();

    for (int fixup_i = 0; fixup_i < machine_code_fixups.size(); ++fixup_i) {
      auto& fixup = machine_code_fixups[fixup_i];
      size_t target_offset = 0;
      if (fixup.target) {                        // target is an instruction
        if (fixup.target->address == nullptr) {  // it wasn't emitted yet - keep the fixup around
          continue;
        }
        target_offset = reinterpret_cast<size_t>(fixup.target->address);
      } else {  // target is not an instruction - create a new exit point and jump there
        target_offset = machine_code_offset;
        EmitExitPoint();
      }
      if (fixup.kind == MCFixupKind::FK_PCRel_4) {
        Fixup4(fixup.end_offset, target_offset);
      } else if (fixup.kind == MCFixupKind::FK_PCRel_1) {
        Fixup1(fixup.end_offset, target_offset);
      } else {
        ERROR << "Unsupported fixup kind: " << fixup.kind;
      }
      machine_code_fixups.EraseIndex(fixup_i);
      --fixup_i;
    }
  }

  if (!machine_code_fixups.empty()) {
    ERROR << "Not all fixups were resolved!";
  }
  string machine_code_str;
  for (int i = 0; i < machine_code_offset; i++) {
    machine_code_str += f("%02x ", machine_code.get()[i]);
    if (i % 16 == 15) {
      machine_code_str += "\n";
    }
  }
  LOG << machine_code_str;

  string epilogue_prologue_str;
  for (int i = 0; i < epilogue_prologue.size(); i++) {
    epilogue_prologue_str += f("%02x ", epilogue_prologue[i]);
    if (i % 16 == 15) {
      epilogue_prologue_str += "\n";
    }
  }
  LOG << epilogue_prologue_str;

#if defined __linux__
  mprotect(machine_code.get(), kMachineCodeSize, PROT_READ | PROT_EXEC);
#else
  // TODO: Implement on Windows
#endif  // __linux__
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
#if defined __linux__
  int STACK_SIZE = 8 * 1024 * 1024;
  std::vector<char> stack;
  stack.resize(STACK_SIZE);
  pid_t v;
  struct ThreadArg {
    PrologueFn prologue_fn;
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
#else
  // TODO: Implement on Windows
#endif  // __linux__
}
}  // namespace automat::library
