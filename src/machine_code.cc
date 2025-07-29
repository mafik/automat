// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#include "machine_code.hh"

#include <llvm/Support/raw_ostream.h>
#include <llvm/lib/Target/X86/X86Subtarget.h>

#include <automat/x86.hh>

#include "build_variant.hh"
#include "format.hh"
#include "llvm_asm.hh"
#include "log.hh"
#include "ptr.hh"

#if defined __linux__
#include <signal.h>
#include <sys/mman.h>  // For mmap related functions and constants
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <ucontext.h>

#include <mutex>
#include <thread>

#include "blockingconcurrentqueue.hh"
#include "thread_name.hh"

#elif defined _WIN32
#include <windows.h>
#endif

namespace automat::mc {

// Switch this to true to see debug logs.
constexpr bool kDebugCodeController = false;

int ImmediateSize(const Inst& inst) { return ::automat::x86::ImmediateSize(inst.getOpcode()); }

struct SignalController;
thread_local SignalController* active_signal_controller = nullptr;

#if defined __linux__
static void SignalHandler(int sig, siginfo_t* si, void* vcontext);
#endif  // __linux__

struct SignalController : Controller {
  std::mutex mutex;

#if defined __linux__
  using ThreadHandle = pid_t;
  using NativeContext = ucontext_t;
  struct sigaction old_usr1;
  std::atomic<ucontext_t*> context{nullptr};

  void InstallSignalHandlers() {
    struct sigaction sa = {};
    sa.sa_sigaction = (void (*)(int, siginfo_t*, void*))SignalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO;
    if (sigaction(SIGUSR1, &sa, &old_usr1) == -1) {
      perror("sigaction");
    }
  }

  void UninstallSignalHandlers() { sigaction(SIGUSR1, &old_usr1, nullptr); }

#define REG(context, reg) context.uc_mcontext.gregs[REG_##reg]
#define NATIVE_RIP(context) REG(context, RIP)
#define NATIVE_RSP(context) REG(context, RSP)

#define FOR_EACH_REG(X) \
  X(RAX, RAX);          \
  X(RBX, RBX);          \
  X(RCX, RCX);          \
  X(RDX, RDX);          \
  X(RBP, RBP);          \
  X(RSI, RSI);          \
  X(RDI, RDI);          \
  X(R8, R8);            \
  X(R9, R9);            \
  X(R10, R10);          \
  X(R11, R11);          \
  X(R12, R12);          \
  X(R13, R13);          \
  X(R14, R14);          \
  X(R15, R15);

#elif defined _WIN32
  using ThreadHandle = HANDLE;
  using NativeContext = CONTEXT;

#define REG(context, reg) context.reg
#define NATIVE_RIP(context) REG(context, Rip)
#define NATIVE_RSP(context) REG(context, Rsp)

#define FOR_EACH_REG(X) \
  X(RAX, Rax);          \
  X(RBX, Rbx);          \
  X(RCX, Rcx);          \
  X(RDX, Rdx);          \
  X(RBP, Rbp);          \
  X(RSI, Rsi);          \
  X(RDI, Rdi);          \
  X(R8, R8);            \
  X(R9, R9);            \
  X(R10, R10);          \
  X(R11, R11);          \
  X(R12, R12);          \
  X(R13, R13);          \
  X(R14, R14);          \
  X(R15, R15);

#endif  // __linux__

  ThreadHandle executing_thread_tid{0};

  Regs regs;

  std::span<char> code;
  // Note: maybe a better calling ABI would be used for assembly? For example preserve_none.
  using PrologueFn = __attribute__((sysv_abi)) uint64_t (*)(uint64_t);
  PrologueFn prologue_fn;
  uint64_t epilogue_address;

  std::vector<NestedWeakPtr<const Inst>> instructions;  // ordered by std::owner_less
  std::vector<int> instruction_offsets;

  // Describes a section of code
  struct MapEntry {
    // Range of memory described by this entry.
    uint32_t begin;
    uint8_t size;

    // Indicates whether this entry describes the instruction body or rather one of its exit
    // points.
    StopType code_type;

    // The Instruction that this section of code belongs to.
    // 0xffff indicates that it's part of the prologue/epilogue.
    uint16_t instruction;
  };

  std::vector<MapEntry> map;

  CodePoint InstructionPointerToCodePoint(uint64_t rip, bool trap) {
    if (rip < (uint64_t)code.data() || rip >= (uint64_t)(code.data() + code.size())) {
      return {nullptr, StopType::InstructionBody};
    }
    uint64_t code_offset = rip - (uint64_t)code.data();
    for (auto& map_entry : map) {
      if (trap) {
        if (map_entry.begin + map_entry.size == code_offset) {
          return {&instructions[map_entry.instruction], map_entry.code_type};
        }
      } else {
        if (map_entry.begin <= code_offset && map_entry.begin + map_entry.size > code_offset) {
          return {&instructions[map_entry.instruction], map_entry.code_type};
        }
      }
    }
    return {nullptr, StopType::InstructionBody};
  }

  uint64_t InstToInstructionPointer(const NestedWeakPtr<const Inst>& inst) {
    auto it = std::lower_bound(instructions.begin(), instructions.end(), inst);
    if (it == instructions.end() || (*it != inst)) {
      return 0;
    }
    int i = std::distance(instructions.begin(), it);
    char* addr = code.data() + instruction_offsets[i];
    return (uint64_t)addr;
  }

  SignalController(ExitCallback&& exit_callback) : Controller(std::move(exit_callback)) {
    constexpr size_t kDefaultMachineCodeSize = 4096;

#if defined __linux__
    auto mem = (char*)mmap((void*)0x10000, kDefaultMachineCodeSize, PROT_READ | PROT_EXEC,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
      return;
    }
#elif _WIN32
    auto mem = (char*)VirtualAlloc(NULL, kDefaultMachineCodeSize, MEM_COMMIT | MEM_RESERVE,
                                   PAGE_EXECUTE_READWRITE);
    if (mem == NULL) {
      return;
    }
#endif  // __linux__

    code = std::span(mem, kDefaultMachineCodeSize);
  }

  ~SignalController() {
    Status status_ignored;
    Cancel(status_ignored);
#if defined __linux__
    munmap(code.data(), code.size());
#elif _WIN32
    VirtualFree(code.data(), code.size(), MEM_RELEASE);
#endif  // __linux__
  }

  void UpdateCode(Program&& program, Status& status) override {
    std::lock_guard<std::mutex> lock(mutex);

    NestedWeakPtr<const Inst> current_instruction;
#if defined _WIN32
    CONTEXT context;
    context.ContextFlags = CONTEXT_INTEGER | CONTEXT_CONTROL;
#endif
    NativeContext* ctx_ptr = nullptr;
    if (executing_thread_tid) {
#if defined __linux__
      syscall(SYS_tgkill, getpid(), executing_thread_tid, SIGUSR1);
      this->context.wait(nullptr);  // wait until assembly thread puts its context
      ctx_ptr = context.load();
#elif defined _WIN32
      SuspendThread(executing_thread_tid);
      GetThreadContext(executing_thread_tid, &context);
      ctx_ptr = &context;
#endif
      auto& ctx_ref = *ctx_ptr;
      auto rip = NATIVE_RIP(ctx_ref);
      auto code_point = InstructionPointerToCodePoint(rip, false);
      if (code_point.instruction) {
        current_instruction = *code_point.instruction;
      }
    }

    // TODO: grow code if necessary
#if defined __linux__
    mprotect(code.data(), code.size(), PROT_READ | PROT_WRITE);
#endif  // __linux__
    // On Windows we keep the code as RWX

    memset(code.data(), 0x90, code.size());

    using namespace llvm;

    SmallVector<char, 256> epilogue_prologue;
    SmallVector<MCFixup, 4> epilogue_prologue_fixups;
    int64_t regs_addr = reinterpret_cast<int64_t>(&regs);

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
#define REGS(x) \
  x(RBX);       \
  x(RCX);       \
  x(RDX);       \
  x(RBP);       \
  x(RSI);       \
  x(RDI);       \
  x(R8);        \
  x(R9);        \
  x(R10);       \
  x(R11);       \
  x(R12);       \
  x(R13);       \
  x(R14);       \
  x(R15);

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

    PUSHr(X86::RDI);  // Push the first argument (RDI) so that it can be used to "RET" into the
                      // right address

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

    prologue_fn = reinterpret_cast<PrologueFn>(reinterpret_cast<intptr_t>(code.data()) +
                                               code.size_bytes() - prologue_size);

    // Copy epilogue/prologue at the end of machine_code.
    void* epilogue_prologue_dest = reinterpret_cast<void*>(
        reinterpret_cast<intptr_t>(code.data()) + code.size_bytes() - epilogue_prologue.size());
    memcpy(epilogue_prologue_dest, epilogue_prologue.data(), epilogue_size + prologue_size);
    epilogue_address = reinterpret_cast<uint64_t>(epilogue_prologue_dest);

    std::string new_code;
    std::vector<MapEntry> new_map;
    std::vector<NestedWeakPtr<const Inst>> new_instructions;
    std::vector<int> new_instruction_offsets(program.size(), -1);

    {  // Fill new_code & new_map
      auto& mc_code_emitter = llvm_asm.mc_code_emitter;
      auto& mc_subtarget_info = *llvm_asm.mc_subtarget_info;

      struct Fixup {
        size_t end_offset;  // place in machine_code where the fixup ends
        int target_index;   // index of the target instruction
        llvm::MCFixupKind kind = llvm::MCFixupKind::FK_PCRel_4;
        int source_index;  // index of the source instruction
        StopType code_type;
      };

      std::vector<Fixup> machine_code_fixups;

      auto EmitInstruction = [&](int instruction_index) {
        llvm::SmallVector<char, 32> instruction_bytes;
        llvm::SmallVector<llvm::MCFixup, 2> instruction_fixups;
        mc_code_emitter->encodeInstruction(*program[instruction_index].inst, instruction_bytes,
                                           instruction_fixups, mc_subtarget_info);

        new_instruction_offsets[instruction_index] = new_code.size();

        new_map.push_back({.begin = (uint32_t)new_code.size(),
                           .size = (uint8_t)instruction_bytes.size(),
                           .code_type = StopType::InstructionBody,
                           .instruction = (uint16_t)instruction_index});

        new_code.append(instruction_bytes.data(), instruction_bytes.size());

        if (instruction_fixups.size() > 1) {
          ERROR << "Instructions with more than one fixup not supported!";
        } else if (instruction_fixups.size() == 1) {
          auto& fixup = instruction_fixups[0];

          machine_code_fixups.push_back({new_code.size(), program[instruction_index].jump,
                                         fixup.getKind(), instruction_index, StopType::Jump});
        }
      };

      auto Fixup1 = [&](size_t fixup_end, size_t target_offset) {
        ssize_t pcrel = (ssize_t)(target_offset) - (ssize_t)(fixup_end);
        new_code[fixup_end - 1] = (pcrel & 0xFF);
      };

      auto Fixup4 = [&](size_t fixup_end, int target_offset) {
        ssize_t pcrel = (ssize_t)(target_offset) - (ssize_t)(fixup_end);
        new_code[fixup_end - 4] = (pcrel & 0xFF);
        new_code[fixup_end - 3] = ((pcrel >> 8) & 0xFF);
        new_code[fixup_end - 2] = ((pcrel >> 16) & 0xFF);
        new_code[fixup_end - 1] = ((pcrel >> 24) & 0xFF);
      };

      auto EmitJump = [&](int instr_index, uint32_t target_offset) {
        llvm::SmallVector<char, 32> instruction_bytes;
        llvm::SmallVector<llvm::MCFixup, 2> instruction_fixups;
        // Save the current RIP and jump to epilogue
        mc_code_emitter->encodeInstruction(llvm::MCInstBuilder(llvm::X86::JMP_4).addImm(0),
                                           instruction_bytes, instruction_fixups,
                                           mc_subtarget_info);
        new_map.push_back({
            .begin = (uint32_t)new_code.size(),
            .size = (uint8_t)instruction_bytes.size(),
            .code_type = StopType::Next,
            .instruction = (uint16_t)instr_index,
        });
        new_code.append(instruction_bytes.data(), instruction_bytes.size());
        Fixup4(new_code.size(), target_offset);
      };

      auto EmitExitPoint = [&](int instr_index, StopType exit_point) {
        new_map.push_back({.begin = (uint32_t)new_code.size(),
                           .size = 1,
                           .code_type = exit_point,
                           .instruction = (uint16_t)instr_index});
        SmallVector<char, 32> instruction_bytes;
        SmallVector<MCFixup, 2> instruction_fixups;
        // Save the current RIP and jump to epilogue
        mc_code_emitter->encodeInstruction(MCInstBuilder(X86::CALL64pcrel32).addImm(0),
                                           instruction_bytes, instruction_fixups,
                                           mc_subtarget_info);
        new_code.append(instruction_bytes.data(), instruction_bytes.size());
        new_map.back().size = instruction_bytes.size();
        Fixup4(new_code.size(), code.size_bytes() - epilogue_prologue.size());
      };

      auto EmitInstructionSequence = [&](int start_i) {
        if (new_instruction_offsets[start_i] >= 0) {
          return;
        }

        int inst_i = start_i;
        int last_inst_i = inst_i;

        while (inst_i >= 0 && inst_i < program.size()) {
          EmitInstruction(inst_i);

          // Follow the "next" connection.
          last_inst_i = inst_i;
          inst_i = program[inst_i].next;
          if (inst_i >= 0 && inst_i < program.size()) {
            int existing_offset = new_instruction_offsets[inst_i];
            if (existing_offset >= 0) {
              EmitJump(last_inst_i, existing_offset);
              return;
            }
          }
        }

        auto& last_inst_info = llvm_asm.mc_instr_info->get(program[last_inst_i].inst->getOpcode());
        if (!last_inst_info.isUnconditionalBranch()) {
          EmitExitPoint(last_inst_i, StopType::Next);
        }
      };

      {  // Emit instruction sequences
        std::vector<int> in_degree(program.size(), 0);
        for (int i = 0; i < program.size(); ++i) {
          if (program[i].next >= 0 && program[i].next < program.size()) {
            in_degree[program[i].next]++;
          }
          if (program[i].jump >= 0 && program[i].jump < program.size()) {
            in_degree[program[i].jump]++;
          }
        }

        std::vector<int> root_indices;
        for (int i = 0; i < program.size(); ++i) {
          if (in_degree[i] == 0) {
            root_indices.push_back(i);
          }
        }

        // First try to emit instruction sequences that have a well-defined starting point.
        for (int root_i : root_indices) {
          EmitInstructionSequence(root_i);
        }

        // Then emit the rest (essentially loops).
        for (int start_i = 0; start_i < program.size(); ++start_i) {
          // Emit a sequence of instructions starting at index `start_i` and following the `next`
          // links.
          EmitInstructionSequence(start_i);
        }
      }

      while (!machine_code_fixups.empty()) {  // Fill fixups
        auto& fixup = machine_code_fixups.back();
        int target_offset;
        if (fixup.target_index >= 0 &&
            fixup.target_index < program.size()) {  // target is an instruction
          target_offset = new_instruction_offsets[fixup.target_index];
        } else {  // target is not an instruction - create a new exit point and jump there
          target_offset = new_code.size();
          EmitExitPoint(fixup.source_index, fixup.code_type);
        }
        if (fixup.kind == llvm::MCFixupKind::FK_PCRel_4) {
          Fixup4(fixup.end_offset, target_offset);
        } else if (fixup.kind == llvm::MCFixupKind::FK_PCRel_1) {
          Fixup1(fixup.end_offset, target_offset);
        } else {
          ERROR << "Unsupported fixup kind: " << fixup.kind;
        }
        machine_code_fixups.pop_back();
      }

      if constexpr (kDebugCodeController) {
        std::string machine_code_str;
        for (int i = 0; i < new_code.size(); i++) {
          machine_code_str += f("{:02x} ", new_code[i]);
          if (i % 16 == 15) {
            machine_code_str += "\n";
          }
        }
        LOG << "New code: " << machine_code_str;
      }
    }

    {  // Fill new_instructions
      new_instructions.reserve(program.size());
      for (auto node : program) {
        new_instructions.emplace_back(node.inst);
      }
    }

    map = std::move(new_map);
    instructions = std::move(new_instructions);
    instruction_offsets = std::move(new_instruction_offsets);

    memcpy(code.data(), new_code.data(), new_code.size());

#if defined __linux__
    mprotect(code.data(), code.size(), PROT_READ | PROT_EXEC);
#endif  // __linux__

    if (executing_thread_tid) {
      auto& ctx_ref = *ctx_ptr;
      NATIVE_RIP(ctx_ref) = InstToInstructionPointer(current_instruction);
#if defined __linux__
      this->context = nullptr;
      this->context.notify_one();
#elif defined _WIN32
      SetThreadContext(executing_thread_tid, ctx_ptr);
      ResumeThread(executing_thread_tid);
#endif
    }
  }

  void Execute(NestedWeakPtr<const Inst> inst, Status& status) override {
    uint64_t rip;
    {  // Pre-entering machine code section
      std::lock_guard<std::mutex> lock(mutex);
      if (executing_thread_tid) {
        // If another thread is already executing, there are multiple potential strategies:
        // 1. Execute in parallel which will totally mess up the state
        // 2. Wait for the other Execute to finish & then execute
        // 3. Cancel the other Execute & then execute ourselves
        // 4. Return an error and let the user decide
        // For now we'll return an error. Eventually we might add options to follow some of the
        // other strategies.
        AppendErrorMessage(status) += "Another thread is already executing";
        return;
      }
      rip = InstToInstructionPointer(inst);
      if (rip == 0) {
        AppendErrorMessage(status) += "Instruction not found in code";
        return;
      }
      active_signal_controller = this;
#if defined __linux__
      executing_thread_tid = gettid();
      InstallSignalHandlers();
#elif defined _WIN32
      DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                      &executing_thread_tid, 0, TRUE, DUPLICATE_SAME_ACCESS);
#endif
    }
    // Machine code section
    auto exit_ptr = prologue_fn(rip);
    {  // Post-exiting machine code section
      std::lock_guard<std::mutex> lock(mutex);
#if defined __linux__
      UninstallSignalHandlers();
#elif defined _WIN32
      CloseHandle(executing_thread_tid);
#endif
      active_signal_controller = nullptr;
      executing_thread_tid = 0;
    }
    auto code_point = InstructionPointerToCodePoint(exit_ptr, true);
    exit_callback(code_point);
  }

  uint64_t StateFromNative(State& state, const NativeContext& context) {
#define X(reg, name) state.regs.reg = REG(context, name);
    FOR_EACH_REG(X);
#undef X
    uint64_t rip = NATIVE_RIP(context);
    auto code_point = InstructionPointerToCodePoint(rip, false);
    if (code_point.instruction) {
      state.current_instruction = *code_point.instruction;
    } else {
      state.current_instruction.Reset();
    }
    return rip;
  }

  void StateToNative(NativeContext& context, const State& state, uint64_t rip) {
#define X(reg, name) REG(context, name) = state.regs.reg;
    FOR_EACH_REG(X);
#undef X
    NATIVE_RIP(context) = rip;
  }

  void GetState(State& state, Status& status) override {
    std::lock_guard<std::mutex> lock(mutex);
    if (executing_thread_tid) {
#if defined __linux__
      syscall(SYS_tgkill, getpid(), executing_thread_tid, SIGUSR1);
      this->context.wait(nullptr);  // wait until assembly thread puts its context
      auto& context = *this->context.load();
#elif defined _WIN32
      int previous_suspend_count = SuspendThread(executing_thread_tid);
      if (previous_suspend_count) {
        AppendErrorMessage(status) +=
            f("Failed to suspend thread {}. Previous suspend count was {}", executing_thread_tid,
              previous_suspend_count);
      }
      CONTEXT context;
      context.ContextFlags = CONTEXT_INTEGER | CONTEXT_CONTROL;
      bool got_context = GetThreadContext(executing_thread_tid, &context);
      if (!got_context) {
        AppendErrorMessage(status) += f("Failed to get thread context for thread {}: {}",
                                        executing_thread_tid, GetLastError());
      }
#endif
      StateFromNative(state, context);
#if defined __linux__
      this->context = nullptr;
      this->context.notify_one();
#elif defined _WIN32
      ResumeThread(executing_thread_tid);
#endif
    } else {
      state.current_instruction.Reset();
      state.regs = regs;
    }
  }

  void ChangeState(StateVisitor visitor, Status& status) override {
    std::lock_guard<std::mutex> lock(mutex);
    State state;
    if (executing_thread_tid) {
#if defined __linux__
      syscall(SYS_tgkill, getpid(), executing_thread_tid, SIGUSR1);
      this->context.wait(nullptr);  // wait until assembly thread puts its context
      auto& context = *this->context.load();
#elif defined _WIN32
      SuspendThread(executing_thread_tid);
      CONTEXT context;
      context.ContextFlags = CONTEXT_INTEGER | CONTEXT_CONTROL;
      GetThreadContext(executing_thread_tid, &context);
#endif
      auto old_rip = StateFromNative(state, context);
      visitor(state);
      uint64_t rip;
      if (state.current_instruction) {
        rip = InstToInstructionPointer(state.current_instruction);
      } else {
        NATIVE_RSP(context) -= 8;
        *(uint64_t*)NATIVE_RSP(context) = old_rip;
        rip = epilogue_address;
      }
      StateToNative(context, state, rip);
#if defined __linux__
      this->context = nullptr;
      this->context.notify_one();
#elif defined _WIN32
      SetThreadContext(executing_thread_tid, &context);
      ResumeThread(executing_thread_tid);
#endif
    } else {
      state.regs = regs;
      visitor(state);
      regs = state.regs;
      if (state.current_instruction) {
        // Not supported yet
        AppendErrorMessage(status) +=
            "ChangeState cannot be used to start assembly execution - this would block!";
      }
    }
  }

  void Cancel(Status& status) override {
    ChangeState([](State& state) { state.current_instruction.Reset(); }, status);
  }
};

#if defined __linux__
static void SignalHandler(int sig, siginfo_t* si, void* vcontext) {
  auto* controller = active_signal_controller;
  if (controller == nullptr) {
    LOG << "SignalHandler: No active signal controller";
    return;
  }
  controller->context.store((ucontext_t*)vcontext);
  controller->context.notify_one();
  controller->context.wait((ucontext_t*)vcontext);
}
#endif  // __linux__

#if defined __linux__

// Uses two threads internally - a worker thread, which executes the actual machine code and control
// thread, which functions as a debugger.
struct PtraceController : Controller {
  using ExitCallback = std::function<void(CodePoint)>;

  // ExitCallback is called on _some_ thread when the machine code reaches an exit point.
  PtraceController(ExitCallback&& exit_callback) : Controller(std::move(exit_callback)) {
    control_thread = std::jthread([this] { this->ControlThread(); });
    // wait for control to stop the worker thread
    worker_set_up.wait(false);
  }

  ~PtraceController() {
    if constexpr (kDebugCodeController) {
      LOG << "User thread: Destroying PtraceMachineCodeController";
    }
    kill(pid, SIGKILL);
    control_commands.enqueue([this]() {
      if constexpr (kDebugCodeController) {
        LOG << "Control thread: Setting controller_running to false";
      }
      this->controller_running = false;
    });
  }

  void UpdateCode(Program&& program, Status& status) override {
    auto& llvm_asm = automat::LLVM_Assembler::Get();

    if constexpr (build_variant::NotRelease) {
      // Verify that the program is sorted by inst.owner_less
      for (int i = 1; i < program.size(); ++i) {
        if (program[i].inst < program[i - 1].inst) {
          AppendErrorMessage(status) += "Instructions are not sorted according to std::owner_less!";
          return;
        }
      }
    }

    if constexpr (kDebugCodeController) {
      LOG << "New instructions:";
      for (int i = 0; i < program.size(); ++i) {
        auto& inst = program[i];
        std::string str;
        llvm::raw_string_ostream os(str);
        llvm_asm.mc_inst_printer->printInst(inst.inst.Get(), 0, "", *llvm_asm.mc_subtarget_info,
                                            os);
        if (inst.next >= 0 && inst.next < program.size()) {
          str += "; next:" + std::to_string(inst.next);
        }
        if (inst.jump >= 0 && inst.jump < program.size()) {
          str += "; jump:" + std::to_string(inst.jump);
        }
        LOG << "  " << i << ": " << str;
      }
    }

    std::string new_code;
    std::vector<MapEntry> new_map;
    std::vector<NestedWeakPtr<const Inst>> new_instructions;
    std::vector<int> new_instruction_offsets(program.size(), -1);

    {  // Fill new_code & new_map
      auto& mc_code_emitter = llvm_asm.mc_code_emitter;
      auto& mc_subtarget_info = *llvm_asm.mc_subtarget_info;

      struct Fixup {
        size_t end_offset;  // place in machine_code where the fixup ends
        int target_index;   // index of the target instruction
        llvm::MCFixupKind kind = llvm::MCFixupKind::FK_PCRel_4;
        int source_index;  // index of the source instruction
        StopType code_type;
      };

      std::vector<Fixup> machine_code_fixups;

      auto EmitInstruction = [&](int instruction_index) {
        llvm::SmallVector<char, 32> instruction_bytes;
        llvm::SmallVector<llvm::MCFixup, 2> instruction_fixups;
        mc_code_emitter->encodeInstruction(*program[instruction_index].inst, instruction_bytes,
                                           instruction_fixups, mc_subtarget_info);

        new_instruction_offsets[instruction_index] = new_code.size();

        new_map.push_back({.begin = (uint32_t)new_code.size(),
                           .size = (uint8_t)instruction_bytes.size(),
                           .code_type = StopType::InstructionBody,
                           .instruction = (uint16_t)instruction_index});

        new_code.append(instruction_bytes.data(), instruction_bytes.size());

        if (instruction_fixups.size() > 1) {
          ERROR << "Instructions with more than one fixup not supported!";
        } else if (instruction_fixups.size() == 1) {
          auto& fixup = instruction_fixups[0];

          machine_code_fixups.push_back({new_code.size(), program[instruction_index].jump,
                                         fixup.getKind(), instruction_index, StopType::Jump});
        }
      };

      auto Fixup1 = [&](size_t fixup_end, size_t target_offset) {
        ssize_t pcrel = (ssize_t)(target_offset) - (ssize_t)(fixup_end);
        new_code[fixup_end - 1] = (pcrel & 0xFF);
      };

      auto Fixup4 = [&](size_t fixup_end, int target_offset) {
        ssize_t pcrel = (ssize_t)(target_offset) - (ssize_t)(fixup_end);
        new_code[fixup_end - 4] = (pcrel & 0xFF);
        new_code[fixup_end - 3] = ((pcrel >> 8) & 0xFF);
        new_code[fixup_end - 2] = ((pcrel >> 16) & 0xFF);
        new_code[fixup_end - 1] = ((pcrel >> 24) & 0xFF);
      };

      auto EmitJump = [&](int instr_index, uint32_t target_offset) {
        llvm::SmallVector<char, 32> instruction_bytes;
        llvm::SmallVector<llvm::MCFixup, 2> instruction_fixups;
        // Save the current RIP and jump to epilogue
        mc_code_emitter->encodeInstruction(llvm::MCInstBuilder(llvm::X86::JMP_4).addImm(0),
                                           instruction_bytes, instruction_fixups,
                                           mc_subtarget_info);
        new_map.push_back({
            .begin = (uint32_t)new_code.size(),
            .size = (uint8_t)instruction_bytes.size(),
            .code_type = StopType::Next,
            .instruction = (uint16_t)instr_index,
        });
        new_code.append(instruction_bytes.data(), instruction_bytes.size());
        Fixup4(new_code.size(), target_offset);
      };

      auto EmitExitPoint = [&](int instr_index, StopType exit_point) {
        new_map.push_back({.begin = (uint32_t)new_code.size(),
                           .size = 1,
                           .code_type = exit_point,
                           .instruction = (uint16_t)instr_index});
        new_code.append("\xcc");
      };

      auto EmitInstructionSequence = [&](int start_i) {
        if (new_instruction_offsets[start_i] >= 0) {
          return;
        }

        int inst_i = start_i;
        int last_inst_i = inst_i;

        while (inst_i >= 0 && inst_i < program.size()) {
          EmitInstruction(inst_i);

          // Follow the "next" connection.
          last_inst_i = inst_i;
          inst_i = program[inst_i].next;
          if (inst_i >= 0 && inst_i < program.size()) {
            int existing_offset = new_instruction_offsets[inst_i];
            if (existing_offset >= 0) {
              EmitJump(last_inst_i, existing_offset);
              return;
            }
          }
        }

        auto& last_inst_info = llvm_asm.mc_instr_info->get(program[last_inst_i].inst->getOpcode());
        if (!last_inst_info.isUnconditionalBranch()) {
          EmitExitPoint(last_inst_i, StopType::Next);
        }
      };

      {  // Emit instruction sequences
        std::vector<int> in_degree(program.size(), 0);
        for (int i = 0; i < program.size(); ++i) {
          if (program[i].next >= 0 && program[i].next < program.size()) {
            in_degree[program[i].next]++;
          }
          if (program[i].jump >= 0 && program[i].jump < program.size()) {
            in_degree[program[i].jump]++;
          }
        }

        std::vector<int> root_indices;
        for (int i = 0; i < program.size(); ++i) {
          if (in_degree[i] == 0) {
            root_indices.push_back(i);
          }
        }

        // First try to emit instruction sequences that have a well-defined starting point.
        for (int root_i : root_indices) {
          EmitInstructionSequence(root_i);
        }

        // Then emit the rest (essentially loops).
        for (int start_i = 0; start_i < program.size(); ++start_i) {
          // Emit a sequence of instructions starting at index `start_i` and following the `next`
          // links.
          EmitInstructionSequence(start_i);
        }
      }

      while (!machine_code_fixups.empty()) {  // Fill fixups
        auto& fixup = machine_code_fixups.back();
        int target_offset;
        if (fixup.target_index >= 0 &&
            fixup.target_index < program.size()) {  // target is an instruction
          target_offset = new_instruction_offsets[fixup.target_index];
        } else {  // target is not an instruction - create a new exit point and jump there
          target_offset = new_code.size();
          EmitExitPoint(fixup.source_index, fixup.code_type);
        }
        if (fixup.kind == llvm::MCFixupKind::FK_PCRel_4) {
          Fixup4(fixup.end_offset, target_offset);
        } else if (fixup.kind == llvm::MCFixupKind::FK_PCRel_1) {
          Fixup1(fixup.end_offset, target_offset);
        } else {
          ERROR << "Unsupported fixup kind: " << fixup.kind;
        }
        machine_code_fixups.pop_back();
      }

      if constexpr (kDebugCodeController) {
        std::string machine_code_str;
        for (int i = 0; i < new_code.size(); i++) {
          machine_code_str += f("{:02x} ", new_code[i]);
          if (i % 16 == 15) {
            machine_code_str += "\n";
          }
        }
        LOG << "New code: " << machine_code_str;
      }
    }

    {  // Fill new_instructions
      new_instructions.reserve(program.size());
      for (auto node : program) {
        new_instructions.emplace_back(node.inst);
      }
    }

    {  // Replace the code, map & instructions on the control thread.
      std::atomic<bool> done = false;
      control_commands.enqueue([this, new_code = std::move(new_code), new_map = std::move(new_map),
                                new_instructions = std::move(new_instructions),
                                new_instruction_offsets = std::move(new_instruction_offsets),
                                &done]() {
        if constexpr (kDebugCodeController) {
          LOG << "Control thread: Replacing code, map & instructions";
        }
        assert(!worker_running);

        {  // Update RIP to keep the currently executing instruction active
          user_regs_struct user_regs;
          if (ptrace(PTRACE_GETREGS, pid, 0, &user_regs) == -1) {
            ERROR << "While reloading the code, PTRACE_GETREGS failed: " << strerror(errno);
            return;
          }

          // Note that code_point.instruction is only valid as long as current `instructions` vector
          // is not modified!!
          auto code_point = InstructionPointerToCodePoint(user_regs.rip, false);

          user_regs.rip = 0;
          if (code_point.instruction) {
            // TODO: there is a subtle bug here - if we pause at an implicitly inserted jmp
            // instruction, then instead of updating the RIP to the next instruction, we'll update
            // it to the instruction prior to the jmp. This will cause it to be executed again.
            auto& old_instr = *code_point.instruction;
            for (auto& map_entry : new_map) {
              auto& new_instr = new_instructions[map_entry.instruction];
              if (new_instr == old_instr) {
                user_regs.rip = (uint64_t)code.data() + map_entry.begin;
                break;
              }
            }
          }
          // TODO: maybe call exit_callback if the new rip is 0?

          if (ptrace(PTRACE_SETREGS, pid, 0, &user_regs) == -1) {
            ERROR << "Couldn't reset RIP after code reload - PTRACE_SETREGS failed: "
                  << strerror(errno);
          }
        }

        // TODO: grow code if necessary
#if defined __linux__
        mprotect(code.data(), code.size(), PROT_READ | PROT_WRITE);
#else
        // TODO: Implement on Windows
#endif  // __linux__

        memcpy(code.data(), new_code.data(), new_code.size());
        bzero(code.data() + new_code.size(), code.size() - new_code.size());

#if defined __linux__
        mprotect(code.data(), code.size(), PROT_READ | PROT_EXEC);
#else
        // TODO: Implement on Windows
#endif  // __linux__

        map = std::move(new_map);
        instructions = std::move(new_instructions);
        instruction_offsets = std::move(new_instruction_offsets);
        done = true;
        done.notify_all();
      });
      WakeControlThread();
      done.wait(false);
    }
  }

  // Start executing machine code at the given instruction.
  //
  // Thread-safe.
  //
  // Although this will actually be executed on a different thread in this implementation of
  // MachineCodeController, keep in mind that other (hypothetical) implementations (e.g.
  // SignalMachineCodeController) may execute the code in a blocking manner, on the current thread.
  void Execute(NestedWeakPtr<const Inst> instr, Status& status) override {
    control_commands.enqueue([this, instr = std::move(instr), &status]() {
      if constexpr (kDebugCodeController) {
        LOG << "Control thread: Executing instruction";
      }
      if (worker_should_run) {
        AppendErrorMessage(status) += "Code is already executing";
        return;
      }
      auto instruction_addr = InstToInstructionPointer(instr);

      // TODO: handle the case where the code already was scheduled for execution
      // (worker_should_run == true)

      int ret;
      user_regs_struct user_regs;
      ret = ptrace(PTRACE_GETREGS, pid, 0, &user_regs);
      if (ret == -1) {
        ERROR << "PTRACE_GETREGS failed: " << strerror(errno);
        return;
      }
      user_regs.rip = instruction_addr;
      ret = ptrace(PTRACE_SETREGS, pid, 0, &user_regs);
      if (ret == -1) {
        ERROR << "PTRACE_SETREGS failed: " << strerror(errno);
        return;
      }
      if constexpr (kDebugCodeController) {
        LOG << "Executing instruction at " << f("{}", reinterpret_cast<void*>(instruction_addr));
      }
      worker_should_run = true;
      ResumeWorker(status);
    });
    WakeControlThread();
  }

  template <typename Lambda>
  void RunOnControlThread(Lambda&& lambda) {
    if (control_thread.get_id() == std::this_thread::get_id()) {
      lambda();
    } else {
      std::atomic<bool> done = false;
      control_commands.enqueue([&]() {
        lambda();
        done = true;
        done.notify_all();
      });
      WakeControlThread();
      done.wait(false);
    }
  }

  // Thread-safe
  void GetState(State& state, Status& status) override {
    auto get_state = [&]() {
      if constexpr (kDebugCodeController) {
        LOG << "Control thread: Getting the state";
      }
      assert(!worker_running);
      uint64_t rip;
      GetRegs(state.regs, rip, status);
      CodePoint code_point = InstructionPointerToCodePoint(rip, false);
      if (code_point.instruction) {
        state.current_instruction = *code_point.instruction;
      } else {
        state.current_instruction.Reset();
      }
    };
    RunOnControlThread(get_state);
  }

  void ChangeState(StateVisitor visitor, Status& status) override {
    RunOnControlThread([this, visitor = std::move(visitor), &status]() {
      if constexpr (kDebugCodeController) {
        LOG << "Control thread: Changing the state";
      }
      assert(!worker_running);
      State state;
      uint64_t rip;
      user_regs_struct user_regs;
      int ret = ptrace(PTRACE_GETREGS, pid, 0, &user_regs);
      if (ret == -1) {
        AppendErrorMessage(status) += "PTRACE_GETREGS(" + f("{}", pid) + ") failed";
        return;
      }
      state.regs.RAX = user_regs.rax;
      state.regs.RBX = user_regs.rbx;
      state.regs.RCX = user_regs.rcx;
      state.regs.RDX = user_regs.rdx;
      state.regs.RBP = user_regs.rbp;
      state.regs.RSI = user_regs.rsi;
      state.regs.RDI = user_regs.rdi;
      state.regs.R8 = user_regs.r8;
      state.regs.R9 = user_regs.r9;
      state.regs.R10 = user_regs.r10;
      state.regs.R11 = user_regs.r11;
      state.regs.R12 = user_regs.r12;
      state.regs.R13 = user_regs.r13;
      state.regs.R14 = user_regs.r14;
      state.regs.R15 = user_regs.r15;
      CodePoint code_point = InstructionPointerToCodePoint(user_regs.rip, false);
      if (code_point.instruction) {
        state.current_instruction = *code_point.instruction;
      }

      visitor(state);

      user_regs.rax = state.regs.RAX;
      user_regs.rbx = state.regs.RBX;
      user_regs.rcx = state.regs.RCX;
      user_regs.rdx = state.regs.RDX;
      user_regs.rbp = state.regs.RBP;
      user_regs.rsi = state.regs.RSI;
      user_regs.rdi = state.regs.RDI;
      user_regs.r8 = state.regs.R8;
      user_regs.r9 = state.regs.R9;
      user_regs.r10 = state.regs.R10;
      user_regs.r11 = state.regs.R11;
      user_regs.r12 = state.regs.R12;
      user_regs.r13 = state.regs.R13;
      user_regs.r14 = state.regs.R14;
      user_regs.r15 = state.regs.R15;
      if (state.current_instruction) {
        user_regs.rip = InstToInstructionPointer(state.current_instruction);
        worker_should_run = true;
      } else {
        worker_should_run = false;
      }
      if (ptrace(PTRACE_SETREGS, pid, 0, &user_regs) == -1) {
        AppendErrorMessage(status) += "PTRACE_SETREGS failed";
      }
    });
  }

  void Cancel(Status&) override {
    control_commands.enqueue([this]() {
      if constexpr (kDebugCodeController) {
        LOG << "Control thread: Cancelling";
      }
      worker_should_run = false;
    });
    WakeControlThread();
  }

 private:
  // Prologue can be called using the following C signature. It enters code at a given `start_addr`.
  // Return value is the address right after the exit point.
  using Prologue = uint64_t (*)(uint64_t start_addr);

  // Range of memory where the machine code is mapped at.
  std::span<char> code = {};

  // PID of the worker thread.
  pid_t pid = 0;

  // Keeps track of the OS state of the worker thread.
  // It's possible that we pause the worker to do some ptrace operation.
  bool worker_running = false;

  // Keeps track of the desired state of the assembler thread.
  // It should be true during:
  // - initialization of the worker thread
  // - between calls to Execute() and [Cancel() or ExitCallback]
  bool worker_should_run = true;

  // Saved state of registers
  Regs regs = {};

  std::vector<NestedWeakPtr<const Inst>> instructions;  // ordered by std::owner_less
  std::vector<int> instruction_offsets;

  // Describes a section of code
  struct MapEntry {
    // Range of memory described by this entry.
    uint32_t begin;
    uint8_t size;

    // Indicates whether this entry describes the instruction body or rather one of its exit
    // points.
    StopType code_type;

    // The Instruction that this section of code belongs to.
    // 0xffff indicates that it's part of the prologue/epilogue.
    uint16_t instruction;
  };

  std::vector<MapEntry> map;

  CodePoint InstructionPointerToCodePoint(uint64_t rip, bool trap) {
    if (rip < (uint64_t)code.data() || rip >= (uint64_t)(code.data() + code.size())) {
      return {nullptr, StopType::InstructionBody};
    }
    uint64_t code_offset = rip - (uint64_t)code.data();
    for (auto& map_entry : map) {
      if (trap) {
        if (map_entry.begin + map_entry.size == code_offset) {
          return {&instructions[map_entry.instruction], map_entry.code_type};
        }
      } else {
        if (map_entry.begin <= code_offset && map_entry.begin + map_entry.size > code_offset) {
          return {&instructions[map_entry.instruction], map_entry.code_type};
        }
      }
    }
    return {nullptr, StopType::InstructionBody};
  }

  uint64_t InstToInstructionPointer(NestedWeakPtr<const Inst> inst) {
    auto it = std::lower_bound(instructions.begin(), instructions.end(), inst);
    if (it == instructions.end() || (*it != inst)) {
      return 0;
    }
    int i = std::distance(instructions.begin(), it);
    char* addr = code.data() + instruction_offsets[i];
    return (uint64_t)addr;
  }

  // Holds commands to be executed on the control thread.
  moodycamel::BlockingConcurrentQueue<std::function<void()>> control_commands;

  std::jthread control_thread;
  bool controller_running = true;
  std::atomic<bool> worker_set_up = false;

  // std::optional < WeakPtr<AutomatInstruction>, CodeType >> nthueo;

  // This should only be called from the control thread.
  void PrintMap() {
    LOG << "Code map:";
    for (auto& map_entry : map) {
      std::string code_type_str;
      switch (map_entry.code_type) {
        case StopType::InstructionBody:
          code_type_str = "InstructionBody";
          break;
        case StopType::Next:
          code_type_str = "Next";
          break;
        case StopType::Jump:
          code_type_str = "Jump";
          break;
        default:
          code_type_str = "Unknown";
          break;
      }
      LOG << "  " << f("{}", map_entry.begin) << "-" << f("{}", map_entry.begin + map_entry.size)
          << " " << code_type_str << " inst=" << map_entry.instruction;
    }
  }

  void WakeControlThread() {
    if (pid) {
      if constexpr (kDebugCodeController) {
        LOG << "Sending SIGUSR1 to control thread " << pid;
      }
      kill(pid, SIGUSR1);
    }
  }

  // This should only be called from the control thread.
  void GetRegs(Regs& regs, uint64_t& rip, Status& status) {
    user_regs_struct user_regs;
    int ret = ptrace(PTRACE_GETREGS, pid, 0, &user_regs);
    if (ret == -1) {
      AppendErrorMessage(status) += "PTRACE_GETREGS(" + f("{}", pid) + ") failed";
      return;
    }
    regs.RAX = user_regs.rax;
    regs.RBX = user_regs.rbx;
    regs.RCX = user_regs.rcx;
    regs.RDX = user_regs.rdx;
    regs.RBP = user_regs.rbp;
    regs.RSI = user_regs.rsi;
    regs.RDI = user_regs.rdi;
    regs.R8 = user_regs.r8;
    regs.R9 = user_regs.r9;
    regs.R10 = user_regs.r10;
    regs.R11 = user_regs.r11;
    regs.R12 = user_regs.r12;
    regs.R13 = user_regs.r13;
    regs.R14 = user_regs.r14;
    regs.R15 = user_regs.r15;
    rip = user_regs.rip;
  }

  // This should only be called from the control thread.
  void SetRegs(Regs& regs, Status& status) {
    user_regs_struct user_regs;
    if (ptrace(PTRACE_GETREGS, pid, 0, &user_regs) == -1) {
      AppendErrorMessage(status) += "SetRegs - PTRACE_GETREGS(" + f("{}", pid) + ") failed";
      return;
    }
    user_regs.rax = regs.RAX;
    user_regs.rbx = regs.RBX;
    user_regs.rcx = regs.RCX;
    user_regs.rdx = regs.RDX;
    user_regs.rbp = regs.RBP;
    user_regs.rsi = regs.RSI;
    user_regs.rdi = regs.RDI;
    user_regs.r8 = regs.R8;
    user_regs.r9 = regs.R9;
    user_regs.r10 = regs.R10;
    user_regs.r11 = regs.R11;
    user_regs.r12 = regs.R12;
    user_regs.r13 = regs.R13;
    user_regs.r14 = regs.R14;
    user_regs.r15 = regs.R15;
    if (ptrace(PTRACE_SETREGS, pid, 0, &user_regs) == -1) {
      AppendErrorMessage(status) += "PTRACE_SETREGS failed";
    }
  }

  // This should only be called from the control thread.
  void ResumeWorker(Status& status) {
    if (worker_running) {
      AppendErrorMessage(status) += "Worker already running";
      return;
    }
    int ret = ptrace(PTRACE_CONT, pid, 0, 0);
    if (ret == -1) {
      AppendErrorMessage(status) += "PTRACE_CONT failed";
      return;
    }
    worker_running = true;
  }

  void ControlThread() {
    SetThreadName("Machine Code Control");
    {  // Allocate memory for code
      constexpr size_t kDefaultMachineCodeSize = PAGE_SIZE;
#if defined __linux__
      auto mem = (char*)mmap((void*)0x10000, kDefaultMachineCodeSize, PROT_READ | PROT_EXEC,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (mem == MAP_FAILED) {
        return;
      }
      code = std::span(mem, kDefaultMachineCodeSize);
#else
      // TODO: Implement on Windows
#endif  // __linux__
    }

    {  // Start worker thread
#if defined __linux__

      int STACK_SIZE = 4 * 1024;
      std::vector<char> stack;
      stack.resize(STACK_SIZE);
      if (clone(WorkerThread, (void*)(stack.data() + STACK_SIZE),
                CLONE_PARENT_SETTID | CLONE_SIGHAND | CLONE_FILES | CLONE_FS | CLONE_IO | CLONE_VM,
                nullptr, &pid) == -1) {
        ERROR << "failed to start machine code worker thread: " << strerror(errno);
        return;
      }
      if constexpr (kDebugCodeController) {
        LOG << "Started worker with PID " << pid;
      }

      int ret = ptrace(PTRACE_SEIZE, pid, 0, PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL);
      if (ret == -1) {
        ERROR << "PTRACE_SEIZE failed: " << strerror(errno);
        return;
      }
      worker_running = true;
#else
       // TODO: Implement on Windows
#endif  // __linux__
    }

    bool initial_registers_set = false;

    while (controller_running) {
      int status;
      std::function<void()> command;
      if (!worker_running) {
        if constexpr (kDebugCodeController) {
          LOG << "Control thread: Worker is stopped, waiting for command";
        }
        control_commands.wait_dequeue(command);
        if constexpr (kDebugCodeController) {
          LOG << "Control thread: Executing command";
          LOG_Indent();
        }
        command();
        if constexpr (kDebugCodeController) {
          LOG_Unindent();
        }
      } else {
        if constexpr (kDebugCodeController) {
          LOG << "Control thread: Worker is running, blocking in waitpid";
        }
        int ret = waitpid(pid, &status, __WALL);
        if (ret == -1) {
          ERROR << "waitpid failed: " << strerror(errno);
          return;
        }
        if (WIFEXITED(status)) {
          if constexpr (kDebugCodeController) {
            LOG << "Control thread: Worker thread exited status=" << WEXITSTATUS(status);
          }
          break;
        } else if (WIFSIGNALED(status)) {
          if constexpr (kDebugCodeController) {
            LOG << "Control thread: Worker thread killed by signal=" << WTERMSIG(status);
          }
          break;
        } else if (WIFSTOPPED(status)) {
          worker_running = false;
          int sig = WSTOPSIG(status);
          if (sig == SIGSTOP) {
            bool group_stop = status >> 16 == PTRACE_EVENT_STOP;
            if (!group_stop) {  // signal-delivery-stop
              // This happens immediately after the worker thread is started.
              // We change this stop state into a group-stop by re-injecting the stop signal.
              if constexpr (kDebugCodeController) {
                LOG << "Control thread: Signal delivery stop - cool, let's keep it stopped";
              }
            } else {  // group-stop
                      // Keep the worker stopped, but listen for other waitpid events.
              if constexpr (kDebugCodeController) {
                LOG << "Control thread: Group-stop for SIGSTOP - little weird but also cool";
              }
            }
            if (!initial_registers_set) {
              initial_registers_set = true;
              worker_should_run = false;
              Regs regs = {};
              Status status;
              uint64_t rip;
              GetRegs(regs, rip, status);
              if (!OK(status)) {
                ERROR << "Couldn't read initial registers: " << status;
                continue;
              }
              regs.RAX = 0;
              regs.RBX = 0;
              regs.RCX = 0;
              regs.RDX = 0;
              regs.RBP = 0;
              regs.RSI = 0;
              regs.RDI = 0;
              regs.R8 = 0;
              regs.R9 = 0;
              regs.R10 = 0;
              regs.R11 = 0;
              regs.R12 = 0;
              regs.R13 = 0;
              regs.R14 = 0;
              regs.R15 = 0;
              SetRegs(regs, status);
              if (!OK(status)) {
                ERROR << "Couldn't initialize starting registers: " << status;
                continue;
              }
              if constexpr (kDebugCodeController) {
                LOG << "Worker thread set up";
              }
              worker_set_up = true;
              worker_set_up.notify_all();
            }
          } else if (sig == SIGUSR1) {
            if constexpr (kDebugCodeController) {
              LOG << "Control thread: Received SIGUSR1 - processing commands";
            }
            // SIGUSR1 means that we probably have some commands to execute.
            // They typically involve stopping the worker thread so instead of resuming the worker
            // and processing the commands while worker is running (which would involve stopping it
            // again), we try to process them immediately here.
            while (control_commands.try_dequeue(command)) {
              command();
            }
            if (worker_should_run) {
              Status status;
              ResumeWorker(status);
              if (!OK(status)) {
                ERROR << "Couldn't resume worker thread: " << status;
                continue;
              }
            }
          } else if (sig == SIGTRAP) {
            if constexpr (kDebugCodeController) {
              LOG << "Received SIGTRAP - calling exit_callback";
            }
            worker_should_run = false;
            if (exit_callback == nullptr) {
              continue;
            }

            user_regs_struct user_regs;
            if (ptrace(PTRACE_GETREGS, pid, 0, &user_regs) == -1) {
              ERROR << "PTRACE_GETREGS failed: " << strerror(errno);
              return;
            }

            if (user_regs.rip < (uint64_t)code.data()) {
              ERROR << "Worker thread was found below the machine code start! RIP="
                    << user_regs.rip;
              continue;
            } else if (user_regs.rip >= (uint64_t)(code.data() + code.size())) {
              ERROR << "Worker thread was found above the machine code end! RIP=" << user_regs.rip;
              continue;
            }

            auto code_point = InstructionPointerToCodePoint(user_regs.rip, true);

            user_regs.rip = 0;
            if (ptrace(PTRACE_SETREGS, pid, 0, &user_regs) == -1) {
              ERROR << "Couldn't reset RIP after trap exit - PTRACE_SETREGS failed: "
                    << strerror(errno);
            }

            exit_callback(code_point);
          } else if (sig == SIGSEGV) {
            siginfo_t siginfo;
            if (ptrace(PTRACE_GETSIGINFO, pid, 0, &siginfo) == -1) {
              ERROR << "PTRACE_GETSIGINFO failed: " << strerror(errno);
              continue;
            }
            ERROR << "Worker thread received SIGSEGV while accessing memory at "
                  << f("{}", siginfo.si_addr);
            continue;
          } else {
            if constexpr (kDebugCodeController) {
              LOG << "Worker thread stopped signal=" << WSTOPSIG(status);
            }
          }
        } else if (WIFCONTINUED(status)) {
          LOG << "Worker thread continued";
        } else {
          ERROR << "waitpid for worker thread returned unknown status: " << status;
          break;
        }
      }
    }

    munmap(code.data(), code.size());
  }

  static int WorkerThread(void* arg) {
    SetThreadName("Machine Code");

    {  // Mask SIGWINCH
       // When the size of terminal changes, it may send a SIGWINCH to the running program.
       // For some reason this signal seems to be delivered to the worker thread.
       // It would unnecessarily stop the execution of the worker (if it was running) and would
       // require us to resume it again immediately. So instead we block the signal altogether.
      sigset_t mask;
      sigemptyset(&mask);
      sigaddset(&mask, SIGWINCH);
      pthread_sigmask(SIG_BLOCK, &mask, nullptr);
    }

    if constexpr (kDebugCodeController) {
      LOG << "Worker thread: Raising SIGSTOP";
    }
    raise(SIGSTOP);
    if constexpr (kDebugCodeController) {
      LOG << "Worker thread: ERROR - resumed at original entry point - quitting";
    }
    return 0;
  }
};

#endif  // __linux__

std::unique_ptr<Controller> Controller::Make(ExitCallback&& exit_callback) {
  return std::make_unique<SignalController>(std::move(exit_callback));
  // Old code:
  // PtraceController* ptrace_controller = new PtraceController(std::move(exit_callback));
  // return std::unique_ptr<Controller>((Controller*)ptrace_controller);
}

}  // namespace automat::mc
