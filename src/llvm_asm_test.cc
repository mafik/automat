// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include <llvm/MC/MCInstBuilder.h>
#include <llvm/lib/Target/X86/X86Subtarget.h>

#include <condition_variable>
#include <csignal>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#include "argument.hh"
#include "base.hh"
#include "gtest.hh"
#include "library_assembler.hh"
#include "library_instruction.hh"
#include "machine_code.hh"
#include "status.hh"

using namespace std::chrono_literals;
using namespace automat;

class MachineCodeControllerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root = MAKE_PTR(Machine);
    auto ExitCallback = [this](mc::CodePoint code_point) {
      std::unique_lock<std::mutex> lock(mutex);
      exit_instr = *code_point.instruction;  // copy the weak_ptr
      exit_point = code_point.stop_type;
      exited = true;
      cv.notify_all();
    };
    controller = mc::Controller::Make(ExitCallback);
  }

  void TearDown() override {
    // Ensure any previous controller is destroyed before starting a new test
    controller.reset();
  }

  void StartExecution(WeakPtr<library::Instruction> instr, bool background_thread = false) {
    exited = false;
    exit_instr = {};
    exit_point = mc::StopType::InstructionBody;
    Status status;
    NestedWeakPtr<const mc::Inst> mc_instr = instr.lock()->ToMC();
    std::atomic<bool> ready = false;
    auto Execute = [&]() {
      ready = true;
      ready.notify_one();
      controller->Execute(mc_instr, status);
    };
    if (background_thread) {
      thread = std::jthread(Execute);
      ready.wait(false);
    } else {
      Execute();
      ASSERT_TRUE(OK(status));
    }
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

  void ExpectWeakPtrsEqual(NestedWeakPtr<const mc::Inst> expected,
                           NestedWeakPtr<const mc::Inst> actual, const std::string& name) {
    auto expected_shared = expected.Lock();
    auto actual_shared = actual.Lock();
    auto expected_ptr = expected_shared.Get();
    auto actual_ptr = actual_shared.Get();
    EXPECT_EQ(expected_ptr, actual_ptr) << name << " ptrs not equal";
  }

  void VerifyState(mc::Controller::State expected,
                   NestedWeakPtr<const mc::Inst> expected_exit_instr = {},
                   mc::StopType expected_exit_point = mc::StopType::InstructionBody) {
    mc::Controller::State state;
    Status status;
    controller->GetState(state, status);
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

  Ptr<library::Instruction> MakeInstructionRegImm(unsigned opcode, unsigned reg, int64_t imm) {
    return MakeInstruction(llvm::MCInstBuilder(opcode).addReg(reg).addImm(imm));
  }

  Ptr<library::Instruction> MakeInstructionImm(unsigned opcode, int64_t imm) {
    return MakeInstruction(llvm::MCInstBuilder(opcode).addImm(imm));
  }

  Ptr<library::Instruction> MakeInstruction(const llvm::MCInst& mc_inst) {
    auto& loc = root->CreateEmpty();
    auto inst = MAKE_PTR(library::Instruction);
    inst->mc_inst = mc_inst;
    inst->mc_inst.setLoc(llvm::SMLoc::getFromPointer((const char*)inst.get()));
    loc.InsertHere(Ptr<library::Instruction>(inst));  // copy Ptr
    return inst;
  }

  Connection* Next(Ptr<library::Instruction>& a, Ptr<library::Instruction>& b) {
    return a->here->ConnectTo(*b->here, next_arg);
  }

  void TestUpdateCode(std::span<library::Instruction*> instructions) {
    std::vector<Ptr<library::Instruction>> instructions_objs;
    for (auto* inst : instructions) {
      instructions_objs.push_back(inst->AcquirePtr());
    }
    Status status;
    UpdateCode(*controller, std::move(instructions_objs), status);
    ASSERT_TRUE(OK(status)) << status.ToStr();
  }

  std::unique_ptr<mc::Controller> controller;
  std::mutex mutex;
  std::condition_variable cv;
  bool exited = false;
  NestedWeakPtr<const mc::Inst> exit_instr = {};
  mc::StopType exit_point = mc::StopType::InstructionBody;
  Ptr<Machine> root;
  std::jthread thread;
};

TEST_F(MachineCodeControllerTest, InitialState) { VerifyState({.regs = {.RAX = 0}}); }

// Checks that a single instruction can be executed correctly
TEST_F(MachineCodeControllerTest, SingleInstruction) {
  auto inst = MakeInstructionRegImm(llvm::X86::MOV64ri, llvm::X86::RAX, 1337);
  library::Instruction* instructions[] = {inst.get()};
  TestUpdateCode(instructions);

  StartExecution(inst);
  ASSERT_TRUE(WaitForExecution());
  VerifyState({.regs = {.RAX = 1337}}, inst->ToMC(), mc::StopType::Next);
}

// Two separate instructions, executed one at a time
TEST_F(MachineCodeControllerTest, TwoSeparateInstructions) {
  auto inst1 = MakeInstructionRegImm(llvm::X86::MOV64ri, llvm::X86::RAX, 1337);
  auto inst2 = MakeInstructionRegImm(llvm::X86::MOV64ri, llvm::X86::RBX, 42);
  library::Instruction* instructions[] = {inst1.get(), inst2.get()};
  TestUpdateCode(instructions);

  StartExecution(inst2);
  ASSERT_TRUE(WaitForExecution());
  VerifyState({.regs = {.RBX = 42}}, inst2->ToMC(), mc::StopType::Next);

  StartExecution(inst1);
  ASSERT_TRUE(WaitForExecution());
  VerifyState({.regs = {.RAX = 1337, .RBX = 42}}, inst1->ToMC(), mc::StopType::Next);
}

// Two instructions, executed one after the other
TEST_F(MachineCodeControllerTest, TwoSequentialInstructions) {
  auto inst1 = MakeInstructionRegImm(llvm::X86::MOV64ri, llvm::X86::RAX, 1337);
  auto inst2 = MakeInstructionRegImm(llvm::X86::MOV64ri, llvm::X86::RBX, 42);
  Next(inst1, inst2);
  library::Instruction* instructions[] = {inst1.get(), inst2.get()};
  TestUpdateCode(instructions);

  StartExecution(inst1);
  ASSERT_TRUE(WaitForExecution());
  VerifyState({.regs = {.RAX = 1337, .RBX = 42}}, inst2->ToMC(), mc::StopType::Next);
}

TEST_F(MachineCodeControllerTest, JumpExitInstruction) {
  // Create a jump instruction not connected to any other instruction.
  // This should trigger a fixup that generates an exit at a Jump exit point.
  auto inst = MakeInstructionImm(llvm::X86::JMP_4, 0);

  library::Instruction* instructions[] = {inst.get()};
  TestUpdateCode(instructions);

  StartExecution(inst);
  ASSERT_TRUE(WaitForExecution());

  // Expect that registers remain zero and that we exit at a Jump exit point.
  VerifyState({.regs = {}}, inst->ToMC(), mc::StopType::Jump);
}

TEST_F(MachineCodeControllerTest, InfiniteLoop) {
  auto inst = MakeInstructionRegImm(llvm::X86::MOV64ri, llvm::X86::RAX, 42);
  Next(inst, inst);
  library::Instruction* instructions[] = {inst.get()};
  TestUpdateCode(instructions);
  StartExecution(inst, true);
  ASSERT_FALSE(WaitForExecution(10ms));

  VerifyState({.current_instruction = inst->ToMC(), .regs = {.RAX = 42}});
  Status status;
  controller->Cancel(status);
  ASSERT_TRUE(WaitForExecution());
}

TEST_F(MachineCodeControllerTest, HotReload) {
  auto inst1 = MakeInstructionRegImm(llvm::X86::MOV64ri, llvm::X86::RAX, 1337);
  auto inst2 = MakeInstructionRegImm(llvm::X86::MOV64ri, llvm::X86::RAX, 42);

  // Start executing inst2 in a loop.
  auto conn = Next(inst2, inst2);
  library::Instruction* instructions[] = {inst1.get(), inst2.get()};
  TestUpdateCode(instructions);
  StartExecution(inst2, true);
  ASSERT_FALSE(WaitForExecution(10ms));
  VerifyState({.current_instruction = inst2->ToMC(), .regs = {.RAX = 42}});

  // Then break the loop by redirecting inst2 to inst1.
  delete conn;
  Next(inst2, inst1);
  library::Instruction* instructions2[] = {inst2.get(), inst1.get()};
  TestUpdateCode(instructions2);
  ASSERT_TRUE(WaitForExecution());
  VerifyState({.regs = {.RAX = 1337}}, inst1->ToMC(), mc::StopType::Next);
}
