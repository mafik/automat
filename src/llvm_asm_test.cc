// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include <csignal>
#include <thread>
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

// Controls the execution of machine code.
//
// Uses two threads internally - a worker thread, which executes the actual machine code and control
// thread, which functions as a debugger.
struct PtraceMachineCodeController {
  using ExitCallback = std::function<void(std::weak_ptr<AutomatInstruction>&, CodeType)>;

  // ExitCallback is called on _some_ thread when the machine code reaches an exit point.
  PtraceMachineCodeController(ExitCallback exit_callback)
      : exit_callback(exit_callback), control_thread([this] { this->ControlThread(); }) {}

  ~PtraceMachineCodeController() {
    kill(pid, SIGKILL);
    control_commands.enqueue([this]() { this->controller_running = false; });
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

      auto EmitJump = [&](uint32_t target_offset) {
        llvm::SmallVector<char, 32> instruction_bytes;
        llvm::SmallVector<llvm::MCFixup, 2> instruction_fixups;
        // Save the current RIP and jump to epilogue
        mc_code_emitter->encodeInstruction(llvm::MCInstBuilder(llvm::X86::JMP_4).addImm(0),
                                           instruction_bytes, instruction_fixups,
                                           mc_subtarget_info);
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

      // TODO: maybe accelerate this with a hash map
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
              EmitJump(existing_offset);
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

      if constexpr (false) {
        std::string machine_code_str;
        for (int i = 0; i < new_code.size(); i++) {
          machine_code_str += maf::f("%02x ", new_code[i]);
          if (i % 16 == 15) {
            machine_code_str += "\n";
          }
        }
        LOG << machine_code_str;
      }
    }

    {  // Fill new_instructions
      new_instructions.reserve(new_instructions_raw.size());
      for (auto inst : new_instructions_raw) {
        new_instructions.emplace_back(inst->WeakPtr());
      }
    }

    {  // Replace the code, map & instructions on the control thread.
      control_commands.enqueue([this, new_code = std::move(new_code), new_map = std::move(new_map),
                                new_instructions = std::move(new_instructions)]() {
      // TODO: handle the case where machine code is currently executing (worker_stopped == false)

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
      });
      LOG << "UpdateCode: Sending SIGUSR1 to worker thread " << pid;
      kill(pid, SIGUSR1);
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
      LOG << "Executing instruction at " << maf::f("%p", instruction_addr);
      worker_stopped = false;
    });
    LOG << "Execute: Sending SIGUSR1 to worker thread " << pid;
    kill(pid, SIGUSR1);
  }

  struct State {
    // Instruction which is about to be executed.
    std::weak_ptr<AutomatInstruction> current_instruction;

    // State of registers prior to the current instruction.
    Regs regs;
  };

  // Thread-safe
  void GetState(State& state) {
    std::atomic<bool> done = false;

    control_commands.enqueue([&]() {
      LOG << "Getting registers...";
      int ret;
      user_regs_struct user_regs;
      ret = ptrace(PTRACE_GETREGS, pid, 0, &user_regs);
      if (ret == -1) {
        ERROR << "GetState: PTRACE_GETREGS failed: " << strerror(errno);
        done = true;
        done.notify_all();
        return;
      }
      state.current_instruction.reset();
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
      done = true;
      done.notify_all();
    });
    kill(pid, SIGUSR1);

    LOG << "Waiting for done...";
    done.wait(false);
    LOG << "Done!";
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

  // Holds commands to be executed on the control thread.
  moodycamel::BlockingConcurrentQueue<std::function<void()>> control_commands;

  ExitCallback exit_callback;
  std::jthread control_thread;
  bool controller_running = true;

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
      LOG << "Started worker with PID " << pid;

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

    while (controller_running) {
      int status;
      std::function<void()> command;
      if (control_commands.try_dequeue(command)) {
        command();
      } else if (worker_stopped) {
        LOG << "Waiting for a new command...";
        control_commands.wait_dequeue(command);
        command();
      } else {
        LOG << "Waiting in waitpid...";
        int ret = waitpid(pid, &status, __WALL);
        if (ret == -1) {
          ERROR << "waitpid failed: " << strerror(errno);
          return;
        }
        if (WIFEXITED(status)) {
          LOG << "Worker thread exited status=" << WEXITSTATUS(status);
          break;
        } else if (WIFSIGNALED(status)) {
          LOG << "Worker thread killed by signal=" << WTERMSIG(status);
          break;
        } else if (WIFSTOPPED(status)) {
          int sig = WSTOPSIG(status);
          if (sig == SIGSTOP) {
            worker_stopped = true;
            bool group_stop = status >> 16 == PTRACE_EVENT_STOP;
            if (!group_stop) {  // signal-delivery-stop
              // This happens immediately after the worker thread is started.
              // We change this stop state into a group-stop by re-injecting the stop signal.
              LOG << "Signal delivery stop - cool, let's keep it stopped";
              // ret = ptrace(PTRACE_CONT, pid, 0, sig);
              // if (ret == -1) {
              //   ERROR << "PTRACE_CONT failed: " << strerror(errno);
              //   return;
              // }
            } else {  // group-stop
                      // Keep the worker stopped, but listen for other waitpid events.

              LOG << "Group-stop for SIGSTOP - little weird but also cool";
            }
          } else if (sig == SIGUSR1) {
            int ret = ptrace(PTRACE_CONT, pid, 0, 0);
            if (ret == -1) {
              ERROR << "PTRACE_CONT after SIGUSR1 failed: " << strerror(errno);
            }
          } else if (sig == SIGTRAP) {
            worker_stopped = true;
            LOG << "Received SIGTRAP - calling exit_callback";
            if (exit_callback == nullptr) {
              continue;
            }

            int ret;
            user_regs_struct user_regs;
            ret = ptrace(PTRACE_GETREGS, pid, 0, &user_regs);
            if (ret == -1) {
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

            uint64_t code_offset = user_regs.rip - (uint64_t)code.data();
            MapEntry* found_map_entry = nullptr;
            for (auto& map_entry : map) {
              // It seems that traps cause the instruction pointer to advance to the next
              // instruction.
              if ((map_entry.code_type == CodeType::Jump ||
                   map_entry.code_type == CodeType::Next) &&
                  (map_entry.begin + map_entry.size == code_offset)) {
                found_map_entry = &map_entry;
                break;
              }
              if (map_entry.begin <= code_offset &&
                  code_offset < map_entry.begin + map_entry.size) {
                found_map_entry = &map_entry;
                break;
              }
            }
            if (found_map_entry) {
              CodeType exit_point = CodeType::InstructionBody;
              if (found_map_entry->code_type == CodeType::Next) {
                exit_point = CodeType::Next;
              } else if (found_map_entry->code_type == CodeType::Jump) {
                exit_point = CodeType::Jump;
              }
              exit_callback(instructions[found_map_entry->instruction], exit_point);
            } else {
              ERROR << "No map entry found for the instruction at RIP="
                    << maf::f("%p", user_regs.rip);
              LOG << "Map entries:";
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
                LOG << "  " << maf::f("%p", map_entry.begin) << "-"
                    << maf::f("%p", map_entry.begin + map_entry.size) << " " << code_type_str << " "
                    << map_entry.instruction;
              }
            }
          } else {
            LOG << "Worker thread stopped signal=" << WSTOPSIG(status);
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
    LOG << "Worker thread started";
    raise(SIGSTOP);
    LOG << "Worker thread exiting";
    return 0;
  }
};

std::shared_ptr<AutomatInstruction> MakeInstruction(const llvm::MCInst& mc_inst) {
  auto inst = std::make_shared<AutomatInstruction>();
  inst->mc_inst = mc_inst;
  return inst;
}

// Checks that a single instruction can be executed correctly
TEST(PtraceMachineCodeController, SingleInstruction) {
  std::mutex mutex;
  std::condition_variable cv;
  bool exited = false;
  std::weak_ptr<AutomatInstruction> exit_instr;
  CodeType exit_point;
  auto exit_callback = [&](std::weak_ptr<AutomatInstruction>& instr, CodeType point) {
    LOG << "Exit callback called!!!";
    std::unique_lock<std::mutex> lock(mutex);
    exit_instr = instr;
    exit_point = point;
    exited = true;
    cv.notify_all();
  };

  PtraceMachineCodeController controller(exit_callback);
  PtraceMachineCodeController::State state;

  auto inst =
      MakeInstruction(llvm::MCInstBuilder(llvm::X86::MOV64ri).addReg(llvm::X86::RAX).addImm(1337));
  AutomatInstruction* instructions[] = {inst.get()};
  controller.UpdateCode(instructions);

  // Verify initial RAX is 0
  controller.GetState(state);
  EXPECT_EQ(state.regs.RAX, 0);

  controller.Execute(inst);
  {
    std::unique_lock<std::mutex> lock(mutex);
    if (!exited) {
      cv.wait_for(lock, 100ms);
    }
  }

  ASSERT_EQ(exited, true);
  EXPECT_EQ(exit_instr.lock(), inst);
  EXPECT_EQ(exit_point, CodeType::Next);

  controller.GetState(state);
  EXPECT_EQ(state.regs.RAX, 1337);
}

// TODO: test case for a sequence of instructions
// TODO: test case for a jump exit
// TODO: test case for a GetStatus during a loop