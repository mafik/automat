// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include <linux/prctl.h>
#include <llvm/IR/Module.h>
#include <llvm/MC/MCAsmInfo.h>
#include <llvm/MC/MCCodeEmitter.h>
#include <llvm/MC/MCContext.h>
#include <llvm/MC/MCInst.h>
#include <llvm/MC/MCInstBuilder.h>
#include <llvm/MC/MCInstPrinter.h>
#include <llvm/MC/MCInstrDesc.h>
#include <llvm/MC/MCInstrInfo.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/lib/Target/X86/MCTargetDesc/X86MCTargetDesc.h>
#include <llvm/lib/Target/X86/X86Subtarget.h>
#include <signal.h>
#include <sys/mman.h>  // For mmap related functions and constants
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/ucontext.h>
#include <sys/uio.h>

#include <csignal>
#include <cstdio>
#include <set>
#include <thread>

#pragma maf add link argument "-lz"
#pragma maf add link argument "-lzstd"

#pragma maf main

using namespace llvm;
using namespace std;

void DumpInfo(const MCInstrInfo* MCI, const MCRegisterInfo* MRI, const MCSubtargetInfo* STI,
              MCInstPrinter* inst_printer) {
  int n = MCI->getNumOpcodes();
  int semanticall_different_op_count = 0;
  printf("const llvm_instrs = [\n");
  for (int i = 0; i < n; i++) {
    bool skip = false;
    std::string desc = "";
    desc += "{\n";
    desc += "  name: \"";
    desc += MCI->getName(i).data();
    desc += "\",\n";

    auto name = MCI->getName(i);

    // TODO: Atomic instructions
    if (name.contains("LOCK_")) skip = true;
    // TODO: EVEX (requires Advanced Performance Extensions - only on newer CPUs - else crash)
    if (name.contains("_NF")) skip = true;
    if (name.contains("_EVEX")) skip = true;
    if (name.contains("_ND")) skip = true;
    if (name.contains("_REV")) skip = true;
    if (name.contains("_alt")) skip = true;

    MCInst inst = MCInstBuilder(i);
    auto mnemonic = inst_printer->getMnemonic(&inst).first;
    if (mnemonic) {
      string mnemonic_str;
      for (int j = 0; mnemonic[j]; ++j) {
        if (isspace(mnemonic[j])) continue;
        mnemonic_str += mnemonic[j];
      }
      desc += "  mnemonic: \"";
      desc += mnemonic_str;
      desc += "\",\n";
    }

    desc += "  opcode: ";
    desc += std::to_string(i);
    desc += ",\n";
    std::string info;
    bool is_deprecated = MCI->getDeprecatedInfo(inst, *STI, info);
    if (is_deprecated) {
      desc += "  deprecated: true,\n";
    }
    const MCInstrDesc& op = MCI->get(i);

    // desc += "Flags=" + std::to_string(op.getFlags()) + " ";
    if (op.isPreISelOpcode()) desc += "  isPreISelOpcode: true,\n";
    if (op.isVariadic()) desc += "  isVariadic: true,\n";
    if (op.hasOptionalDef()) desc += "  hasOptionalDef: true,\n";
    if (op.isPseudo()) {
      desc += "  isPseudo: true,\n";
      skip = true;
    }
    if (op.isMetaInstruction()) desc += "  isMetaInstruction: true,\n";
    if (op.isReturn()) desc += "  isReturn: true,\n";
    if (op.isAdd()) desc += "  isAdd: true,\n";
    if (op.isTrap()) desc += "  isTrap: true,\n";
    if (op.isMoveReg()) desc += "  isMoveReg: true,\n";
    if (op.isCall()) desc += "  isCall: true,\n";
    if (op.isBarrier()) desc += "  isBarrier: true,\n";
    if (op.isTerminator()) desc += "  isTerminator: true,\n";
    if (op.isBranch()) desc += "  isBranch: true,\n";
    if (op.isIndirectBranch()) desc += "  isIndirectBranch: true,\n";
    if (op.isConditionalBranch()) desc += "  isConditionalBranch: true,\n";
    if (op.isUnconditionalBranch()) desc += "  isUnconditionalBranch: true,\n";
    // if (op.mayAffectControlFlow(mov_rax_2, *MRI)) desc += "mayAffectControlFlow ";
    if (op.isPredicable()) desc += "  isPredicable: true,\n";
    if (op.isCompare()) desc += "  isCompare: true,\n";
    if (op.isMoveImmediate()) desc += "  isMoveImmediate: true,\n";
    if (op.isBitcast()) desc += "  isBitcast: true,\n";
    if (op.isSelect()) desc += "  isSelect: true,\n";
    if (op.isNotDuplicable()) desc += "  isNotDuplicable: true,\n";
    if (op.hasDelaySlot()) desc += "  hasDelaySlot: true,\n";
    if (op.canFoldAsLoad()) desc += "  canFoldAsLoad: true,\n";
    if (op.isRegSequenceLike()) desc += "  isRegSequenceLike: true,\n";
    if (op.isExtractSubregLike()) desc += "  isExtractSubregLike: true,\n";
    if (op.isInsertSubregLike()) desc += "  isInsertSubregLike: true,\n";
    if (op.isConvergent()) desc += "  isConvergent: true,\n";
    if (op.variadicOpsAreDefs()) desc += "  variadicOpsAreDefs: true,\n ";
    if (op.isAuthenticated()) desc += "  isAuthenticated: true,\n";
    if (op.mayLoad()) desc += "  mayLoad: true,\n";
    if (op.mayStore()) desc += "  mayStore: true,\n";
    if (op.mayRaiseFPException()) desc += "  mayRaiseFPException: true,\n";
    if (op.hasUnmodeledSideEffects()) desc += "  hasUnmodeledSideEffects: true,\n";
    if (op.isCommutable()) desc += "  isCommutable: true,\n";
    if (op.isConvertibleTo3Addr()) desc += "  isConvertibleTo3Addr: true,\n";
    if (op.usesCustomInsertionHook()) desc += "  usesCustomInsertionHook: true,\n";
    if (op.hasPostISelHook()) desc += "  hasPostISelHook: true,\n";
    if (op.isRematerializable()) desc += "  isRematerializable: true,\n";
    if (op.isAsCheapAsAMove()) desc += "  isAsCheapAsAMove: true,\n";
    if (op.hasExtraDefRegAllocReq()) desc += "  hasExtraDefRegAllocReq: true,\n";
    if (op.hasExtraSrcRegAllocReq()) desc += "  hasExtraSrcRegAllocReq: true,\n";
    if (op.getSize()) desc += "  size: " + std::to_string(op.getSize()) + ",\n";
    // Note: nice function: hasDefOfPhysReg

    int num_operands = op.getNumOperands();
    // TODO: print operands
    // TODO: implicit_uses(), implicit_defs()
    std::string operands_info = "  operands: [\n";
    auto operands = op.operands();
    for (int operand_i = 0; operand_i < operands.size(); ++operand_i) {
      auto& operand = operands[operand_i];
      std::string operand_info = "    {";
      // operand_info += "index:";
      // operand_info += std::to_string(operand_i);
      // operand_info += ",";
      if (operand.OperandType == MCOI::OPERAND_REGISTER) {
        if (operand.isLookupPtrRegClass()) {
          operand_info += "isLookupPtrRegClass:true,";
        } else {
          const MCRegisterClass& reg_class = MRI->getRegClass(operand.RegClass);
          auto reg_class_name = MRI->getRegClassName(&reg_class);
          operand_info += "regClass:\"";
          operand_info += reg_class_name;
          operand_info += "\",";
        }
      } else if (operand.OperandType == MCOI::OPERAND_IMMEDIATE) {
        operand_info += "isImmediate:true,";
      } else if (operand.OperandType == MCOI::OPERAND_MEMORY) {
        operand_info += "isMemory:true,";
      } else if (operand.OperandType == MCOI::OPERAND_PCREL) {
        operand_info += "isPCRel:true,";
      } else if (operand.isGenericType()) {
        operand_info += "genericTypeIndex:";
        operand_info += std::to_string(operand.getGenericTypeIndex());
        operand_info += ",";
      } else if (operand.isGenericImm()) {
        operand_info += "genericImmIndex:";
        operand_info += std::to_string(operand.getGenericImmIndex());
        operand_info += ",";
      } else {
        // probably target-specific operand type
        // TODO: investigate
        operand_info += "unknown:true,";
      }
      if (operand.isPredicate()) operand_info += "isPredicate:true,";
      if (operand.isOptionalDef()) operand_info += "isOptionalDef:true,";
      if (operand.isBranchTarget()) operand_info += "isBranchTarget:true,";
      if (auto tied_to = op.getOperandConstraint(operand_i, MCOI::OperandConstraint::TIED_TO);
          tied_to != -1) {
        operand_info += "tiedTo:";
        operand_info += std::to_string(tied_to);
        operand_info += ",";
      }
      if (auto early_clobber =
              op.getOperandConstraint(operand_i, MCOI::OperandConstraint::EARLY_CLOBBER);
          early_clobber != -1) {
        operand_info += "earlyClobber:true,";
      }
      if (operand_info.ends_with(",")) operand_info.pop_back();
      operands_info += operand_info + "},\n";
    }
    if (operands_info.ends_with('\n')) operands_info.pop_back();
    if (operands_info.ends_with(",")) operands_info.pop_back();
    if (operands_info.ends_with("[")) {
      operands_info += "],\n";
    } else {
      operands_info += "\n  ],\n";
    }
    desc += operands_info;

    std::string implicit_info = "  implicit_defs: [";
    auto implicit_defs = op.implicit_defs();
    for (auto& def : implicit_defs) {
      if (!implicit_info.ends_with("[")) implicit_info += ",";
      implicit_info += "\"";
      implicit_info += MRI->getName(def);
      implicit_info += "\"";
    }
    implicit_info += "],\n";
    implicit_info += "  implicit_uses: [";
    for (auto& use : op.implicit_uses()) {
      if (!implicit_info.ends_with("[")) implicit_info += ",";
      implicit_info += "\"";
      implicit_info += MRI->getName(use);
      implicit_info += "\"";
    }
    implicit_info += "],\n";
    desc += implicit_info;
    if (desc.ends_with('\n')) desc.pop_back();
    if (desc.ends_with(",")) desc.pop_back();
    desc += "\n }";
    if (i != n - 1)
      desc += ",";
    else
      desc += "\n";
    if (skip) {
      continue;
    }
    printf(" %s", desc.c_str());
    ++semanticall_different_op_count;
  }
  printf("];\n");
}

void signal_handler(int sig, siginfo_t* si, struct ucontext_t* context) {
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

void SetupSignalHandlerStack() {
  stack_t ss;
  ss.ss_size = SIGSTKSZ;
  char alt_stack[ss.ss_size];
  ss.ss_sp = alt_stack;
  ss.ss_flags = 0;
  sigaltstack(&ss, nullptr);
}

void SetupSignalHandler() {
  struct sigaction sa;
  memset(&sa, 0, sizeof(struct sigaction));
  // sigemptyset(&sa.sa_mask);
  sigfillset(&sa.sa_mask);
  sa.sa_sigaction = (void (*)(int, siginfo_t*, void*))signal_handler;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;

  if (sigaction(SIGSEGV, &sa, NULL) == -1) {
    perror("Failed to set SIGSEGV handler");
    return;
  }
  if (sigaction(SIGILL, &sa, NULL) == -1) {
    perror("Failed to set SIGILL handler");
    return;
  }
  if (sigaction(SIGBUS, &sa, NULL) == -1) {
    perror("Failed to set SIGBUS handler");
    return;
  }
}

struct InstructionObject {
  MCInst inst;
};

int main() {
  // Initialize target
  LLVMInitializeX86TargetInfo();
  LLVMInitializeX86Target();
  LLVMInitializeX86TargetMC();
  LLVMInitializeX86AsmPrinter();

  SetupSignalHandler();

  // Create target machine
  std::string Error;
  const Target* TheTarget = TargetRegistry::lookupTarget("x86_64-pc-linux-gnu", Error);
  if (!TheTarget) {
    fprintf(stderr, "Target lookup failed: %s\n", Error.c_str());
    return 1;
  }

  TargetOptions Options;
  auto TM = std::unique_ptr<TargetMachine>(
      TheTarget->createTargetMachine("x86_64-pc-linux-gnu", "generic", "", Options, nullopt));

  const MCAsmInfo* MAI = TM->getMCAsmInfo();
  auto MCI = TM->getMCInstrInfo();
  auto MRI = TM->getMCRegisterInfo();
  auto STI = TM->getMCSubtargetInfo();
  auto MIP =
      TheTarget->createMCInstPrinter(Triple("x86_64-pc-linux-gnu"), 1 /*Intel*/, *MAI, *MCI, *MRI);

  DumpInfo(MCI, MRI, STI, MIP);
  return 0;

  // Create MC context
  MCContext Ctx(Triple("x86_64-pc-linux-gnu"), MAI, MRI, STI);

  auto mc_inst_printer =
      TheTarget->createMCInstPrinter(Triple("x86_64-pc-linux-gnu"), 1, *MAI, *MCI, *MRI);

#define REGS(CB) \
  CB(RBX)        \
  CB(RCX)        \
  CB(RDX)        \
  CB(RSP)        \
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
#define CB(reg) uint64_t reg;
    ALL_REGS(CB);
#undef CB
    uint64_t original_RSP;
  };

  alignas(64) Regs regs{};
  void* regs_ptr = &regs;
  int64_t regs_ptr_val = reinterpret_cast<int64_t>(regs_ptr);

  auto PrintRegs = [&]() {
#define PRINT(reg) printf("%8s", #reg);
    ALL_REGS(PRINT);
#undef PRINT
    printf("\n");
#define PRINT(reg) printf("%8lx", regs.reg);
    ALL_REGS(PRINT);
#undef PRINT
    printf("\n  original_rsp: %lx\n", regs.original_RSP);
  };

  // Create instructions
  std::vector<MCInst> insts;
  {
    using I = MCInstBuilder;

    // Save callee-saved registers
    insts.push_back(I(X86::PUSH64r).addReg(X86::RBX));
    insts.push_back(I(X86::PUSH64r).addReg(X86::RBP));
    insts.push_back(I(X86::PUSH64r).addReg(X86::R12));
    insts.push_back(I(X86::PUSH64r).addReg(X86::R13));
    insts.push_back(I(X86::PUSH64r).addReg(X86::R14));
    insts.push_back(I(X86::PUSH64r).addReg(X86::R15));

    // Load regs from memory
    auto PrepareLoadSave = [&]() {
      insts.push_back(I(X86::MOV64ri).addReg(X86::RAX).addImm(regs_ptr_val));
    };

    auto Load = [&](MCRegister reg, int offset) {
      insts.push_back(I(X86::MOV64rm)
                          .addReg(reg)
                          .addReg(X86::RAX)
                          .addImm(1)
                          .addReg(0)
                          .addImm(offset)
                          .addReg(0));
    };
    auto Save = [&](MCRegister reg, int offset) {
      insts.push_back(I(X86::MOV64mr)
                          .addReg(X86::RAX)
                          .addImm(1)
                          .addReg(0)
                          .addImm(offset)
                          .addReg(0)
                          .addReg(reg));
    };
    PrepareLoadSave();
    Save(X86::RSP, offsetof(Regs, original_RSP));
#define LOAD(reg) Load(X86::reg, offsetof(Regs, reg));
    REGS(LOAD);
    LOAD(RAX);  // load RAX last because it's used as a base for address
#undef LOAD

    // Increment RAX and RBX, add them into RAX
    insts.push_back(I(X86::INC64r).addReg(X86::RAX).addReg(X86::RAX));
    insts.push_back(I(X86::INC64r).addReg(X86::RBX).addReg(X86::RBX));
    insts.push_back(I(X86::ADD64rr).addReg(X86::RAX).addReg(X86::RAX).addReg(X86::RBX));

    // Infinite loop
    insts.push_back(I(X86::JMP_1).addImm(0));

    // SIGSEGV
    insts.push_back(I(X86::MOV64rm)
                        .addReg(X86::RCX)
                        .addReg(X86::NoRegister)  // base
                        .addImm(1)                // index multiplicand (1/2/4/8)
                        .addReg(X86::NoRegister)  // index
                        .addImm(0x12)             // displacement
                        .addReg(0));              // segment override

    // SIGILL
    // insts.push_back(I(X86::UD1Qr).addReg(X86::RAX).addReg(X86::RAX));

    // BuildMI()
    // Save regs to memory
    // Special instruction that stores RAX without messing up any registers
    insts.push_back(I(X86::MOV64o64a).addImm(regs_ptr_val).addReg(0));

    PrepareLoadSave();

#define SAVE(reg) Save(X86::reg, offsetof(Regs, reg));
    REGS(SAVE);
#undef SAVE
    Load(X86::RSP, offsetof(Regs, original_RSP));

    // Restore callee-saved registers
    insts.push_back(I(X86::POP64r).addReg(X86::R15));
    insts.push_back(I(X86::POP64r).addReg(X86::R14));
    insts.push_back(I(X86::POP64r).addReg(X86::R13));
    insts.push_back(I(X86::POP64r).addReg(X86::R12));
    insts.push_back(I(X86::POP64r).addReg(X86::RBP));
    insts.push_back(I(X86::POP64r).addReg(X86::RBX));

    // Return
    insts.push_back(I(X86::RET32));
  }

  // Create emitter and encode instructions
  std::unique_ptr<MCCodeEmitter> MCE(TheTarget->createMCCodeEmitter(*MCI, Ctx));

  SmallVector<char, 128> CodeBytes;

  for (auto& inst : insts) {
    SmallVector<MCFixup, 4> Fixups;
    size_t base_offset = CodeBytes.size();
    MCE->encodeInstruction(inst, CodeBytes, Fixups, *STI);

    for (size_t i = 0; i < Fixups.size(); ++i) {
      auto& fixup = Fixups[i];
      if (fixup.getKind() == MCFixupKind::FK_PCRel_1) {
        size_t offset = base_offset + fixup.getOffset();
        CodeBytes[offset] = (char)-2;
      }
    }
  }

  // Print machine code as hex
  printf("Machine code:\n  ");
  for (char byte : CodeBytes) {
    printf("%02x ", (unsigned char)byte);
  }
  printf("\n");

  // Allocate executable memory and copy code
  void* Memory = mmap((void*)0x1000, CodeBytes.size(), PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  // printf("memory: %p\n", Memory);
  memcpy(Memory, CodeBytes.data(), CodeBytes.size());

  // Execute code
  void (*Func)() = (void (*)())Memory;

  printf("Before:\n");
  PrintRegs();

  int STACK_SIZE = 8 * 1024 * 1024;
  char* vstack = (char*)malloc(STACK_SIZE);
  pid_t v;
  struct ThreadArg {
    void (*Func)();
  };
  auto Thread = [](void* void_arg) {
    ThreadArg* arg = (ThreadArg*)void_arg;
    SetupSignalHandlerStack();
    prctl(PR_SET_DUMPABLE, 1);

    printf("Starting infinite loop\n");
    arg->Func();
    printf("Infinite loop done\n");  // we shouldn't ever see this
    return 0;
  };
  // auto x = clone3;
  ThreadArg arg = {Func};
  if (clone(Thread, (void*)(vstack + STACK_SIZE),
            CLONE_PARENT_SETTID | CLONE_SIGHAND | CLONE_FILES | CLONE_FS | CLONE_IO | CLONE_VM,
            &arg, &v) == -1) {
    perror("failed to spawn child task");
    return 3;
  }

  printf("t1_pid: %d\n", v);
  std::this_thread::sleep_for(1s);

  // Goal: fetch registers from t1
  // Step 1: PTRACE_SEIZE

  int ret = ptrace(PTRACE_SEIZE, v, 0, PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL);
  if (ret != 0) {
    perror("PTRACE_SEIZE failed");
    return 1;
  }

  printf("PTRACE_SEIZE done\n");

  for (int i = 0; i < 10; ++i) {
    ret = ptrace(PTRACE_INTERRUPT, v, 0, 0);
    if (ret != 0) {
      perror("PTRACE_INTERRUPT failed");
      return 1;
    }
    printf("PTRACE_INTERRUPT done\n");

    char buf[1024];
    iovec iov;
    iov.iov_base = buf;
    iov.iov_len = sizeof(buf);

    ret = ptrace(PTRACE_GETREGSET, v, 1, &iov);
    if (ret != 0) {
      perror("PTRACE_GETREGSET failed");
      return 1;
    }
    printf("Regs (%d B):\n", iov.iov_len);
    for (int i = 0; i < iov.iov_len; ++i) {
      printf("%02x ", (unsigned char)buf[i]);
    }
    printf("\n");

    ret = ptrace(PTRACE_CONT, v, 0, 0);
    if (ret != 0) {
      perror("PTRACE_CONT failed");
      return 1;
    }
    printf("PTRACE_CONT done\n");
    // sleep for 1s
    std::this_thread::sleep_for(1s);
  }

  printf("After func invoked:\n");
  PrintRegs();

  // Cleanup
  munmap(Memory, CodeBytes.size());

  return 0;
}
