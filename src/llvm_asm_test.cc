// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include <llvm/MC/MCInstBuilder.h>
#include <llvm/lib/Target/X86/X86Subtarget.h>

#include <condition_variable>
#include <mutex>
#include <span>
#include <vector>

#include "concurrentqueue.hh"
#include "gtest.hh"

using namespace llvm;
using namespace std::chrono_literals;

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

enum class ExitPoint {
  Next,
  Jump,
};

// Controls the execution of machine code.
//
// Uses two threads internally - a worker thread, which executes the actual machine code and control
// thread, which functions as a debugger.
template <class InstructionT>
struct MachineCodeController {
  using ExitCallback = std::function<void(std::weak_ptr<InstructionT>, ExitPoint)>;

  MachineCodeController(ExitCallback exit_callback)
      : exit_callback(exit_callback), control_thread(ControlThread, this) {
    // TODO: map a page of memory & mark it as executable
    // TODO: start the control thread
  }

  ~MachineCodeController() {
    // TODO: stop the control thread
    // TODO: unmap memory
  }

  // Convert the given instructions into machine code, hot-reloading if necessary.
  // Thread-safe.
  void InstructionsUpdated(std::span<InstructionT*> instructions) {
    // TODO: put stuff from UpdateMachineCode here
  }

  // Start executing machine code at the given instruction.
  // Thread-safe.
  void Execute(std::shared_ptr<InstructionT>&) {
    // TODO
  }

  struct State {
    std::weak_ptr<InstructionT> current_instruction;
    Regs regs;
  };

  void GetState(State&) {
    // TODO:
  }

 private:
  // Prologue can be called using the following C signature. It enters code at a given `start_addr`.
  // Return value is the address right after the exit point.
  using Prologue = uint64_t (*)(uint64_t start_addr);

  // Range of memory where the machine code is mapped at.
  std::span<char> code = {};

  // Saved state of registers
  Regs regs = {};

  std::vector<std::weak_ptr<InstructionT>> instructions;  // ordered by std::owner_less

  // Describes a section of code
  struct MapEntry {
    // Range of memory described by this entry.
    uint32_t begin;
    uint8_t size;

    // Indicates whether this entry describes the instruction body or rather one of its exit points.
    enum : uint8_t { kInstructionBody, kExitPointNext, kExitPointJump } argument;

    // The Instruction that this section of code belongs to.
    // 0xffff indicates that it's part of the prologue/epilogue.
    uint16_t instruction;
  };

  std::vector<MapEntry> map;

  // Holds commands to be executed on the control thread.
  moodycamel::ConcurrentQueue<std::function<void()>> control_commands;

  ExitCallback exit_callback;
  std::jthread control_thread;

  static void ControlThread(MachineCodeController*) {
    // TODO: start worker thread
    // TODO: attach it and stop
    // TODO: enter a loop waiting for commands from other threads
    // TODO: kill the worker thread once finished
  }

  static void WorkerThread(void* arg) {
    // TODO: traceme
  }
};

struct MockInstruction {
  MCInst mc_inst;
};

std::shared_ptr<MockInstruction> MakeInstruction(const MCInst& mc_inst) {
  auto inst = std::make_shared<MockInstruction>();
  inst->mc_inst = mc_inst;
  return inst;
}

// Checks that a simple instruction can be executed
TEST(LLVMAsm, SetRAX) {
  std::mutex mutex;
  std::condition_variable cv;
  bool exited = false;
  std::weak_ptr<MockInstruction> exit_instr;
  ExitPoint exit_point;
  auto exit_callback = [&](std::weak_ptr<MockInstruction> instr, ExitPoint point) {
    std::unique_lock<std::mutex> lock(mutex);
    exit_instr = instr;
    exit_point = point;
    exited = true;
    cv.notify_all();
  };

  MachineCodeController<MockInstruction> controller(nullptr);
  MachineCodeController<MockInstruction>::State state;

  auto inst = MakeInstruction(MCInstBuilder(X86::MOV64ri).addReg(X86::RAX).addImm(1337));
  MockInstruction* instructions[] = {inst.get()};
  controller.InstructionsUpdated(instructions);

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
  EXPECT_EQ(exit_point, ExitPoint::Next);
}

// TODO: test case for a sequence of instructions
// TODO: test case for a jump exit