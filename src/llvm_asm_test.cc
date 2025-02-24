// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include <csignal>
#include <thread>

#include "argument.hh"
#include "base.hh"
#include "status.hh"

#if defined __linux__
#include <signal.h>
#include <sys/mman.h>  // For mmap related functions and constants
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/wait.h>
#endif  // __linux__

#include <llvm/MC/MCInstBuilder.h>
#include <llvm/lib/Target/X86/X86Subtarget.h>

#include <condition_variable>
#include <mutex>
#include <span>
#include <vector>

#include "blockingconcurrentqueue.hh"
#include "gtest.hh"
#include "library_instruction.hh"
#include "llvm_asm.hh"
#include "thread_name.hh"

using namespace std::chrono_literals;

using AutomatInstruction = automat::library::Instruction;

constexpr bool kDebugCodeController = false;

// Note that RSP is not preserved!
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

// Holds the value of registers between invocations of MachineCode
struct Regs {
#define CB(reg) uint64_t reg = 0;
  ALL_REGS(CB);
#undef CB
  uint64_t operator[](int index) { return reinterpret_cast<uint64_t*>(this)[index]; }
};

enum class CodeType : uint8_t {
  InstructionBody,
  Next,
  Jump,
};

struct CodePoint {
  std::weak_ptr<AutomatInstruction>* instruction;
  CodeType code_type;
};

// Controls the execution of machine code.
//
// Uses two threads internally - a worker thread, which executes the actual machine code and control
// thread, which functions as a debugger.
struct PtraceMachineCodeController {
  using ExitCallback = std::function<void(CodePoint)>;

  // ExitCallback is called on _some_ thread when the machine code reaches an exit point.
  PtraceMachineCodeController(ExitCallback exit_callback)
      : exit_callback(exit_callback), control_thread([this] { this->ControlThread(); }) {
    // wait for control to stop the worker thread
    worker_set_up.wait(false);
  }

  ~PtraceMachineCodeController() {
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

  // Convert the given instructions into machine code, hot-reloading if necessary.
  //
  // Thread-safe.
  void UpdateCode(std::span<AutomatInstruction*> new_instructions_raw) {
    std::string new_code;
    std::vector<MapEntry> new_map;
    std::vector<std::weak_ptr<AutomatInstruction>> new_instructions;

    {  // Fill new_code & new_map
      auto& llvm_asm = automat::LLVM_Assembler::Get();
      auto& mc_code_emitter = llvm_asm.mc_code_emitter;
      auto& mc_subtarget_info = *llvm_asm.mc_subtarget_info;

      // Find all the x86 instructions
      struct InstructionEntry {
        automat::Location* loc;
        automat::library::Instruction* inst;
      };
      // Important! Same order as new_instructions_raw!
      maf::Vec<InstructionEntry> instruction_entries;
      for (auto* inst : new_instructions_raw) {
        instruction_entries.push_back(InstructionEntry{
            .loc = inst->here.lock().get(),
            .inst = inst,
        });
      }

      struct Fixup {
        size_t end_offset;  // place in machine_code where the fixup ends
        automat::library::Instruction* target = nullptr;
        llvm::MCFixupKind kind = llvm::MCFixupKind::FK_PCRel_4;
        int instruction_index;
        CodeType code_type;
      };

      maf::Vec<Fixup> machine_code_fixups;

      auto EmitInstruction = [&](int instruction_index, automat::Location* loc,
                                 automat::library::Instruction& inst) {
        llvm::SmallVector<char, 32> instruction_bytes;
        llvm::SmallVector<llvm::MCFixup, 2> instruction_fixups;
        mc_code_emitter->encodeInstruction(inst.mc_inst, instruction_bytes, instruction_fixups,
                                           mc_subtarget_info);

        new_map.push_back({.begin = (uint32_t)new_code.size(),
                           .size = (uint8_t)instruction_bytes.size(),
                           .code_type = CodeType::InstructionBody,
                           .instruction = (uint16_t)instruction_index});

        new_code.append(instruction_bytes.data(), instruction_bytes.size());

        if (instruction_fixups.size() > 1) {
          ERROR << "Instructions with more than one fixup not supported!";
        } else if (instruction_fixups.size() == 1) {
          auto& fixup = instruction_fixups[0];

          automat::library::Instruction* jump_inst = nullptr;
          if (loc) {
            if (auto it = loc->outgoing.find(&automat::library::jump_arg);
                it != loc->outgoing.end()) {
              jump_inst = (*it)->to.As<automat::library::Instruction>();
            }
          }
          // We assume that the fixup is at the end of the instruction.
          machine_code_fixups.push_back(
              {new_code.size(), jump_inst, fixup.getKind(), instruction_index, CodeType::Jump});
        }
      };

      auto Fixup1 = [&](size_t fixup_end, size_t target_offset) {
        ssize_t pcrel = (ssize_t)(target_offset) - (ssize_t)(fixup_end);
        new_code[fixup_end - 1] = (pcrel & 0xFF);
      };

      auto Fixup4 = [&](size_t fixup_end, size_t target_offset) {
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
            .code_type = CodeType::Next,
            .instruction = (uint16_t)instr_index,
        });
        new_code.append(instruction_bytes.data(), instruction_bytes.size());
        Fixup4(new_code.size(), target_offset);
      };

      auto EmitExitPoint = [&](int instr_index, CodeType exit_point) {
        new_map.push_back({.begin = (uint32_t)new_code.size(),
                           .size = 1,
                           .code_type = exit_point,
                           .instruction = (uint16_t)instr_index});
        new_code.append("\xcc");
      };

      // TODO: accelerate this by putting the instruction body index in the `instructions` vector
      auto OffsetOfInstructionIndex = [&](int instruction_index) -> uint32_t {
        for (auto& map_entry : new_map) {
          if ((map_entry.code_type == CodeType::InstructionBody) &&
              (map_entry.instruction == instruction_index)) {
            return map_entry.begin;
          }
        }
        return UINT32_MAX;
      };

      // TODO: accelerate this with a hash map
      auto OffsetOfInstruction = [&](automat::library::Instruction* inst) -> uint32_t {
        for (auto& map_entry : new_map) {
          if (map_entry.instruction == UINT16_MAX) {
            continue;
          }
          if ((map_entry.code_type == CodeType::InstructionBody) &&
              (new_instructions_raw[map_entry.instruction] == inst)) {
            return map_entry.begin;
          }
        }
        return UINT32_MAX;
      };

      auto IndexOfInstruction = [&](automat::library::Instruction* inst) -> int {
        for (int i = 0; i < new_instructions_raw.size(); ++i) {
          if (new_instructions_raw[i] == inst) {
            return i;
          }
        }
        return -1;
      };

      for (int entry_i = 0; entry_i < instruction_entries.size(); ++entry_i) {
        auto& entry = instruction_entries[entry_i];
        automat::Location* loc = entry.loc;
        automat::library::Instruction* inst = entry.inst;

        if (OffsetOfInstructionIndex(entry_i) != UINT32_MAX) {
          continue;
        }

        int inst_i = entry_i;
        int last_inst_i = inst_i;

        while (inst) {
          EmitInstruction(inst_i, loc, *inst);

          // Follow the "next" connection.
          last_inst_i = inst_i;
          if (loc == nullptr) {
            inst = nullptr;
          } else if (auto it = loc->outgoing.find(&automat::next_arg); it != loc->outgoing.end()) {
            loc = &(*it)->to;
            inst = loc->As<automat::library::Instruction>();
          } else {
            loc = nullptr;
            inst = nullptr;
          }
          if (inst) {
            inst_i = IndexOfInstruction(inst);
            uint32_t existing_offset = OffsetOfInstructionIndex(inst_i);
            if (existing_offset != UINT32_MAX) {
              EmitJump(last_inst_i, existing_offset);
              break;
            }
          }
        }
        // TODO: don't emit the final exit point if the last instruction is a terminator
        EmitExitPoint(last_inst_i, CodeType::Next);
      }

      while (!machine_code_fixups.empty()) {
        auto& fixup = machine_code_fixups.back();
        size_t target_offset = 0;
        if (fixup.target) {  // target is an instruction
          target_offset = OffsetOfInstruction(fixup.target);
        } else {  // target is not an instruction - create a new exit point and jump there
          target_offset = new_code.size();
          EmitExitPoint(fixup.instruction_index, fixup.code_type);
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
          machine_code_str += maf::f("%02x ", new_code[i]);
          if (i % 16 == 15) {
            machine_code_str += "\n";
          }
        }
        LOG << "New code: " << machine_code_str;
      }
    }

    {  // Fill new_instructions
      new_instructions.reserve(new_instructions_raw.size());
      for (auto inst : new_instructions_raw) {
        new_instructions.emplace_back(inst->WeakPtr());
      }
    }

    {  // Replace the code, map & instructions on the control thread.
      std::atomic<bool> done = false;
      control_commands.enqueue([this, new_code = std::move(new_code), new_map = std::move(new_map),
                                new_instructions = std::move(new_instructions), &done]() {
        if constexpr (kDebugCodeController) {
          LOG << "Control thread: Replacing code, map & instructions";
        }
        assert(worker_stopped);

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
              if (!new_instr.owner_before(old_instr) && !old_instr.owner_before(new_instr)) {
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
  void Execute(std::weak_ptr<AutomatInstruction> instr) {
    control_commands.enqueue([this, instr = std::move(instr)]() {
      if constexpr (kDebugCodeController) {
        LOG << "Control thread: Executing instruction";
      }
      int instruction_index = -1;
      for (int i = 0; i < instructions.size(); ++i) {
        if (!instructions[i].owner_before(instr) && !instr.owner_before(instructions[i])) {
          instruction_index = i;
          break;
        }
      }
      if (instruction_index == -1) {
        ERROR << "Instruction not found";
        return;
      }
      int map_index = -1;
      for (int i = 0; i < map.size(); ++i) {
        if (map[i].instruction == instruction_index &&
            map[i].code_type == CodeType::InstructionBody) {
          map_index = i;
          break;
        }
      }
      if (map_index == -1) {
        ERROR << "Instruction not found in the code map";
        return;
      }

      char* instruction_addr = code.data() + map[map_index].begin;

      // TODO: handle the case where the code is executing (worker_stopped == false)

      // int status;
      // int ret = waitpid(pid, &status, 0);
      // if (ret == -1) {
      //   ERROR << "waitpid failed: " << strerror(errno);
      //   return;
      // }
      int ret;
      user_regs_struct user_regs;
      ret = ptrace(PTRACE_GETREGS, pid, 0, &user_regs);
      if (ret == -1) {
        ERROR << "PTRACE_GETREGS failed: " << strerror(errno);
        return;
      }
      user_regs.rip = (uint64_t)instruction_addr;
      ret = ptrace(PTRACE_SETREGS, pid, 0, &user_regs);
      if (ret == -1) {
        ERROR << "PTRACE_SETREGS failed: " << strerror(errno);
        return;
      }
      ret = ptrace(PTRACE_CONT, pid, 0, 0);
      if (ret == -1) {
        ERROR << "PTRACE_CONT failed: " << strerror(errno);
        return;
      }
      if constexpr (kDebugCodeController) {
        LOG << "Executing instruction at " << maf::f("%p", instruction_addr);
      }
      worker_stopped = false;
    });
    WakeControlThread();
  }

  struct State {
    // Instruction which is about to be executed.
    std::weak_ptr<AutomatInstruction> current_instruction;

    // State of registers prior to the current instruction.
    Regs regs;
  };

  // Thread-safe (except for the control thread)
  void GetState(State& state) {
    std::atomic<bool> done = false;
    control_commands.enqueue([&]() {
      if constexpr (kDebugCodeController) {
        LOG << "Control thread: Getting the state";
      }
      assert(worker_stopped);
      state.current_instruction.reset();
      maf::Status status;
      uint64_t rip;
      GetRegs(state.regs, rip, status);
      if (!OK(status)) {
        ERROR << status;
      }

      auto code_point = InstructionPointerToCodePoint(rip, false);
      if (code_point.instruction) {
        state.current_instruction = *code_point.instruction;
      }
      done = true;
      done.notify_all();
    });
    WakeControlThread();
    done.wait(false);
  }

 private:
  // Prologue can be called using the following C signature. It enters code at a given `start_addr`.
  // Return value is the address right after the exit point.
  using Prologue = uint64_t (*)(uint64_t start_addr);

  // Range of memory where the machine code is mapped at.
  std::span<char> code = {};

  // PID of the worker thread.
  pid_t pid = 0;

  bool worker_stopped = false;

  // Saved state of registers
  Regs regs = {};

  std::vector<std::weak_ptr<AutomatInstruction>> instructions;  // ordered by std::owner_less

  // Describes a section of code
  struct MapEntry {
    // Range of memory described by this entry.
    uint32_t begin;
    uint8_t size;

    // Indicates whether this entry describes the instruction body or rather one of its exit
    // points.
    CodeType code_type;

    // The Instruction that this section of code belongs to.
    // 0xffff indicates that it's part of the prologue/epilogue.
    uint16_t instruction;
  };

  std::vector<MapEntry> map;

  CodePoint InstructionPointerToCodePoint(uint64_t rip, bool trap) {
    if (rip < (uint64_t)code.data() || rip >= (uint64_t)(code.data() + code.size())) {
      return {nullptr, CodeType::InstructionBody};
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
    return {nullptr, CodeType::InstructionBody};
  }

  // Holds commands to be executed on the control thread.
  moodycamel::BlockingConcurrentQueue<std::function<void()>> control_commands;

  ExitCallback exit_callback;
  std::jthread control_thread;
  bool controller_running = true;
  std::atomic<bool> worker_set_up = false;

  // std::optional < std::weak_ptr<AutomatInstruction>, CodeType >> nthueo;

  // This should only be called from the control thread.
  void PrintMap() {
    LOG << "Code map:";
    for (auto& map_entry : map) {
      std::string code_type_str;
      switch (map_entry.code_type) {
        case CodeType::InstructionBody:
          code_type_str = "InstructionBody";
          break;
        case CodeType::Next:
          code_type_str = "Next";
          break;
        case CodeType::Jump:
          code_type_str = "Jump";
          break;
        default:
          code_type_str = "Unknown";
          break;
      }
      LOG << "  " << maf::f("%d", map_entry.begin) << "-"
          << maf::f("%d", map_entry.begin + map_entry.size) << " " << code_type_str
          << " inst=" << map_entry.instruction;
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
  void GetRegs(Regs& regs, uint64_t& rip, maf::Status& status) {
    user_regs_struct user_regs;
    int ret = ptrace(PTRACE_GETREGS, pid, 0, &user_regs);
    if (ret == -1) {
      maf::AppendErrorMessage(status) += "PTRACE_GETREGS(" + maf::f("%d", pid) + ") failed";
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
  void SetRegs(Regs& regs, maf::Status& status) {
    user_regs_struct user_regs;
    if (ptrace(PTRACE_GETREGS, pid, 0, &user_regs) == -1) {
      maf::AppendErrorMessage(status) +=
          "SetRegs - PTRACE_GETREGS(" + maf::f("%d", pid) + ") failed";
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
      maf::AppendErrorMessage(status) += "PTRACE_SETREGS failed";
    }
  }

  // This should only be called from the control thread.
  void ResumeWorker(maf::Status& status) {
    if (!worker_stopped) {
      maf::AppendErrorMessage(status) += "Worker already running";
      return;
    }
    int ret = ptrace(PTRACE_CONT, pid, 0, 0);
    if (ret == -1) {
      maf::AppendErrorMessage(status) += "PTRACE_CONT failed";
      return;
    }
    worker_stopped = false;
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
      worker_stopped = false;
#else
       // TODO: Implement on Windows
#endif  // __linux__
    }

    bool initial_registers_set = false;

    while (controller_running) {
      int status;
      std::function<void()> command;
      if (control_commands.try_dequeue(command)) {
        if constexpr (kDebugCodeController) {
          LOG << "Control thread: Found command to execute, worker_stopped=" << worker_stopped;
        }
        command();
      } else if (worker_stopped) {
        if constexpr (kDebugCodeController) {
          LOG << "Control thread: Worker is stopped, waiting for command";
        }
        control_commands.wait_dequeue(command);
        if constexpr (kDebugCodeController) {
          LOG << "Control thread: Executing command";
        }
        command();
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
          worker_stopped = true;
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
              Regs regs = {};
              maf::Status status;
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
            maf::Status status;
            ResumeWorker(status);
            if (!OK(status)) {
              ERROR << "Couldn't resume worker thread: " << status;
              continue;
            }
          } else if (sig == SIGTRAP) {
            if constexpr (kDebugCodeController) {
              LOG << "Received SIGTRAP - calling exit_callback";
            }
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
                  << maf::f("%p", siginfo.si_addr);
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

class PtraceMachineCodeControllerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root = std::make_shared<automat::Machine>();
    auto ExitCallback = [this](CodePoint code_point) {
      std::unique_lock<std::mutex> lock(mutex);
      exit_instr = *code_point.instruction;  // copy the weak_ptr
      exit_point = code_point.code_type;
      exited = true;
      cv.notify_all();
    };
    controller = std::make_unique<PtraceMachineCodeController>(ExitCallback);
  }

  void TearDown() override {
    // Ensure any previous controller is destroyed before starting a new test
    controller.reset();
  }

  void StartExecution(std::weak_ptr<AutomatInstruction> instr) {
    exited = false;
    exit_instr = {};
    exit_point = CodeType::InstructionBody;
    controller->Execute(instr);
  }

  // Waits for machine code execution to complete, with timeout
  // Returns true if execution completed, false if timeout occurred
  bool WaitForExecution(std::chrono::milliseconds timeout = 100ms) {
    std::unique_lock<std::mutex> lock(mutex);
    if (!exited) {
      return cv.wait_for(lock, timeout, [this]() { return exited; });
    }
    return true;
  }

  void ExpectWeakPtrsEqual(std::weak_ptr<AutomatInstruction> expected,
                           std::weak_ptr<AutomatInstruction> actual, const std::string& name) {
    auto expected_shared = expected.lock();
    auto actual_shared = actual.lock();
    auto expected_ptr = expected_shared.get();
    auto actual_ptr = actual_shared.get();
    EXPECT_EQ(expected_ptr, actual_ptr) << name << " ptrs not equal";
  }

  void VerifyState(PtraceMachineCodeController::State expected,
                   std::weak_ptr<AutomatInstruction> expected_exit_instr = {},
                   CodeType expected_exit_point = CodeType::InstructionBody) {
    PtraceMachineCodeController::State state;
    controller->GetState(state);
    ExpectWeakPtrsEqual(expected.current_instruction, state.current_instruction,
                        "current_instruction");
    EXPECT_EQ(state.regs.RAX, expected.regs.RAX);
    EXPECT_EQ(state.regs.RBX, expected.regs.RBX);
    EXPECT_EQ(state.regs.RCX, expected.regs.RCX);
    EXPECT_EQ(state.regs.RDX, expected.regs.RDX);
    EXPECT_EQ(state.regs.RBP, expected.regs.RBP);
    EXPECT_EQ(state.regs.RSI, expected.regs.RSI);
    EXPECT_EQ(state.regs.RDI, expected.regs.RDI);
    EXPECT_EQ(state.regs.R8, expected.regs.R8);
    EXPECT_EQ(state.regs.R9, expected.regs.R9);
    EXPECT_EQ(state.regs.R10, expected.regs.R10);
    EXPECT_EQ(state.regs.R11, expected.regs.R11);
    EXPECT_EQ(state.regs.R12, expected.regs.R12);
    EXPECT_EQ(state.regs.R13, expected.regs.R13);
    EXPECT_EQ(state.regs.R14, expected.regs.R14);
    EXPECT_EQ(state.regs.R15, expected.regs.R15);
    ExpectWeakPtrsEqual(expected_exit_instr, exit_instr, "exit_instr");
    EXPECT_EQ(exit_point, expected_exit_point);
  }

  std::shared_ptr<AutomatInstruction> MakeInstructionRegImm(unsigned opcode, unsigned reg,
                                                            int64_t imm) {
    return MakeInstruction(llvm::MCInstBuilder(opcode).addReg(reg).addImm(imm));
  }

  std::shared_ptr<AutomatInstruction> MakeInstructionImm(unsigned opcode, int64_t imm) {
    return MakeInstruction(llvm::MCInstBuilder(opcode).addImm(imm));
  }

  std::shared_ptr<AutomatInstruction> MakeInstruction(const llvm::MCInst& mc_inst) {
    auto& loc = root->CreateEmpty();
    auto inst = std::make_shared<AutomatInstruction>();
    inst->mc_inst = mc_inst;
    loc.InsertHereNoWidget(std::shared_ptr<AutomatInstruction>(inst));  // copy shared_ptr
    return inst;
  }

  automat::Connection* Next(std::shared_ptr<AutomatInstruction>& a,
                            std::shared_ptr<AutomatInstruction>& b) {
    return a->here.lock()->ConnectTo(*b->here.lock(), automat::next_arg);
  }

  std::unique_ptr<PtraceMachineCodeController> controller;
  std::mutex mutex;
  std::condition_variable cv;
  bool exited = false;
  std::weak_ptr<AutomatInstruction> exit_instr = {};
  CodeType exit_point = CodeType::InstructionBody;
  std::shared_ptr<automat::Machine> root;
};

TEST_F(PtraceMachineCodeControllerTest, InitialState) { VerifyState({.regs = {.RAX = 0}}); }

// Checks that a single instruction can be executed correctly
TEST_F(PtraceMachineCodeControllerTest, SingleInstruction) {
  auto inst = MakeInstructionRegImm(llvm::X86::MOV64ri, llvm::X86::RAX, 1337);
  AutomatInstruction* instructions[] = {inst.get()};
  controller->UpdateCode(instructions);

  StartExecution(inst);
  ASSERT_TRUE(WaitForExecution());
  VerifyState({.regs = {.RAX = 1337}}, inst, CodeType::Next);
}

// Two separate instructions, executed one at a time
TEST_F(PtraceMachineCodeControllerTest, TwoSeparateInstructions) {
  auto inst1 = MakeInstructionRegImm(llvm::X86::MOV64ri, llvm::X86::RAX, 1337);
  auto inst2 = MakeInstructionRegImm(llvm::X86::MOV64ri, llvm::X86::RBX, 42);
  AutomatInstruction* instructions[] = {inst1.get(), inst2.get()};
  controller->UpdateCode(instructions);

  StartExecution(inst2);
  ASSERT_TRUE(WaitForExecution());
  VerifyState({.regs = {.RBX = 42}}, inst2, CodeType::Next);

  StartExecution(inst1);
  ASSERT_TRUE(WaitForExecution());
  VerifyState({.regs = {.RAX = 1337, .RBX = 42}}, inst1, CodeType::Next);
}

// Two instructions, executed one after the other
TEST_F(PtraceMachineCodeControllerTest, TwoSequentialInstructions) {
  auto inst1 = MakeInstructionRegImm(llvm::X86::MOV64ri, llvm::X86::RAX, 1337);
  auto inst2 = MakeInstructionRegImm(llvm::X86::MOV64ri, llvm::X86::RBX, 42);
  Next(inst1, inst2);
  AutomatInstruction* instructions[] = {inst1.get(), inst2.get()};
  controller->UpdateCode(instructions);

  StartExecution(inst1);
  ASSERT_TRUE(WaitForExecution());
  VerifyState({.regs = {.RAX = 1337, .RBX = 42}}, inst2, CodeType::Next);
}

TEST_F(PtraceMachineCodeControllerTest, JumpExitInstruction) {
  // Create a jump instruction not connected to any other instruction.
  // This should trigger a fixup that generates an exit at a Jump exit point.
  auto inst = MakeInstructionImm(llvm::X86::JMP_4, 0);

  AutomatInstruction* instructions[] = {inst.get()};
  controller->UpdateCode(instructions);

  StartExecution(inst);
  ASSERT_TRUE(WaitForExecution());

  // Expect that registers remain zero and that we exit at a Jump exit point.
  VerifyState({.regs = {}}, inst, CodeType::Jump);
}

TEST_F(PtraceMachineCodeControllerTest, InfiniteLoop) {
  auto inst = MakeInstructionRegImm(llvm::X86::MOV64ri, llvm::X86::RAX, 42);
  Next(inst, inst);
  AutomatInstruction* instructions[] = {inst.get()};
  controller->UpdateCode(instructions);
  StartExecution(inst);
  ASSERT_FALSE(WaitForExecution(10ms));

  VerifyState({.current_instruction = inst, .regs = {.RAX = 42}});
}

TEST_F(PtraceMachineCodeControllerTest, HotReload) {
  auto inst1 = MakeInstructionRegImm(llvm::X86::MOV64ri, llvm::X86::RAX, 1337);
  auto inst2 = MakeInstructionRegImm(llvm::X86::MOV64ri, llvm::X86::RAX, 42);

  // Start executing inst2 in a loop.
  auto conn = Next(inst2, inst2);
  AutomatInstruction* instructions[] = {inst1.get(), inst2.get()};
  controller->UpdateCode(instructions);
  StartExecution(inst2);
  ASSERT_FALSE(WaitForExecution(10ms));
  VerifyState({.current_instruction = inst2, .regs = {.RAX = 42}});

  // Then break the loop by redirecting inst2 to inst1.
  delete conn;
  Next(inst2, inst1);
  AutomatInstruction* instructions2[] = {inst2.get(), inst1.get()};
  controller->UpdateCode(instructions2);
  ASSERT_TRUE(WaitForExecution());
  VerifyState({.regs = {.RAX = 1337}}, inst1, CodeType::Next);
}
