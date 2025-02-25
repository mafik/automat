// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include <llvm/MC/MCInstBuilder.h>
#include <llvm/lib/Target/X86/X86Subtarget.h>

#include <condition_variable>
#include <csignal>
#include <mutex>
#include <span>
#include <vector>

#include "argument.hh"
#include "base.hh"
#include "gtest.hh"
#include "library_instruction.hh"
#include "machine_code.hh"
#include "status.hh"

using namespace std::chrono_literals;

// Convenience function for updating the code with a vector of automat::library::Instruction.
//
// The main purpose is to allow Controller to store the instruction pointers without
// having to know about the automat::Instruction type. This is achieved using the _aliasing_
// feature of std::shared_ptr. The shared_ptr used by the controller points to mc_inst field of
// automat::Instruction, while owning the whole object.
void UpdateCode(automat::mc::Controller& controller,
                std::vector<std::shared_ptr<automat::library::Instruction>>&& instructions,
                maf::Status& status) {
  // Sorting allows us to more efficiently search for instructions.
  using comp = std::owner_less<std::shared_ptr<automat::library::Instruction>>;
  std::sort(instructions.begin(), instructions.end(), comp{});

  int n = instructions.size();

  automat::mc::Program program;
  program.resize(n);

  for (int i = 0; i < n; ++i) {
    automat::library::Instruction* obj_raw = instructions[i].get();
    const automat::mc::Inst* inst_raw = &obj_raw->mc_inst;
    auto* loc = obj_raw->here.lock().get();
    int next = -1;
    int jump = -1;
    if (loc) {
      auto FindInstruction = [loc, &instructions, n](const automat::Argument& arg) -> int {
        if (auto it = loc->outgoing.find(&arg); it != loc->outgoing.end()) {
          automat::Location* to_loc = &(*it)->to;
          if (auto to_inst = to_loc->As<automat::library::Instruction>()) {
            auto it = std::lower_bound(instructions.begin(), instructions.end(), to_loc->object,
                                       std::owner_less{});
            if (it != instructions.end()) {
              return std::distance(instructions.begin(), it);
            }
          }
        }
        return -1;
      };
      next = FindInstruction(automat::next_arg);
      jump = FindInstruction(automat::library::jump_arg);
    }
    program[i].next = next;
    program[i].jump = jump;
  }
  for (int i = 0; i < n; ++i) {
    automat::library::Instruction* obj_raw = instructions[i].get();
    const automat::mc::Inst* inst_raw = &obj_raw->mc_inst;
    program[i].inst =
        std::shared_ptr<const automat::mc::Inst>(std::move(instructions[i]), inst_raw);
  }

  controller.UpdateCode(std::move(program), status);
}

std::weak_ptr<automat::mc::Inst> ToMC(automat::library::Instruction& instr) {
  auto* mc_inst = &instr.mc_inst;
  return std::shared_ptr<automat::mc::Inst>(std::move(instr.SharedPtr()), mc_inst);
}

class MachineCodeControllerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root = std::make_shared<automat::Machine>();
    auto ExitCallback = [this](automat::mc::CodePoint code_point) {
      std::unique_lock<std::mutex> lock(mutex);
      exit_instr = *code_point.instruction;  // copy the weak_ptr
      exit_point = code_point.code_type;
      exited = true;
      cv.notify_all();
    };
    controller = automat::mc::Controller::Make(ExitCallback);
  }

  void TearDown() override {
    // Ensure any previous controller is destroyed before starting a new test
    controller.reset();
  }

  void StartExecution(std::weak_ptr<automat::library::Instruction> instr) {
    exited = false;
    exit_instr = {};
    exit_point = automat::mc::CodeType::InstructionBody;
    maf::Status status;
    std::weak_ptr<automat::mc::Inst> mc_instr = ToMC(*instr.lock());
    controller->Execute(mc_instr, status);
    ASSERT_TRUE(OK(status));
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

  void ExpectWeakPtrsEqual(std::weak_ptr<const automat::mc::Inst> expected,
                           std::weak_ptr<const automat::mc::Inst> actual, const std::string& name) {
    auto expected_shared = expected.lock();
    auto actual_shared = actual.lock();
    auto expected_ptr = expected_shared.get();
    auto actual_ptr = actual_shared.get();
    EXPECT_EQ(expected_ptr, actual_ptr) << name << " ptrs not equal";
  }

  void VerifyState(
      automat::mc::Controller::State expected,
      std::weak_ptr<automat::mc::Inst> expected_exit_instr = {},
      automat::mc::CodeType expected_exit_point = automat::mc::CodeType::InstructionBody) {
    automat::mc::Controller::State state;
    maf::Status status;
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

  std::shared_ptr<automat::library::Instruction> MakeInstructionRegImm(unsigned opcode,
                                                                       unsigned reg, int64_t imm) {
    return MakeInstruction(llvm::MCInstBuilder(opcode).addReg(reg).addImm(imm));
  }

  std::shared_ptr<automat::library::Instruction> MakeInstructionImm(unsigned opcode, int64_t imm) {
    return MakeInstruction(llvm::MCInstBuilder(opcode).addImm(imm));
  }

  std::shared_ptr<automat::library::Instruction> MakeInstruction(const llvm::MCInst& mc_inst) {
    auto& loc = root->CreateEmpty();
    auto inst = std::make_shared<automat::library::Instruction>();
    inst->mc_inst = mc_inst;
    inst->mc_inst.setLoc(llvm::SMLoc::getFromPointer((const char*)inst.get()));
    loc.InsertHereNoWidget(
        std::shared_ptr<automat::library::Instruction>(inst));  // copy shared_ptr
    return inst;
  }

  automat::Connection* Next(std::shared_ptr<automat::library::Instruction>& a,
                            std::shared_ptr<automat::library::Instruction>& b) {
    return a->here.lock()->ConnectTo(*b->here.lock(), automat::next_arg);
  }

  void TestUpdateCode(std::span<automat::library::Instruction*> instructions) {
    std::vector<std::shared_ptr<automat::library::Instruction>> instructions_objs;
    for (auto* inst : instructions) {
      instructions_objs.push_back(inst->SharedPtr());
    }
    maf::Status status;
    UpdateCode(*controller, std::move(instructions_objs), status);
    ASSERT_TRUE(OK(status)) << status.ToStr();
  }

  std::unique_ptr<automat::mc::Controller> controller;
  std::mutex mutex;
  std::condition_variable cv;
  bool exited = false;
  std::weak_ptr<const automat::mc::Inst> exit_instr = {};
  automat::mc::CodeType exit_point = automat::mc::CodeType::InstructionBody;
  std::shared_ptr<automat::Machine> root;
};

TEST_F(MachineCodeControllerTest, InitialState) { VerifyState({.regs = {.RAX = 0}}); }

// Checks that a single instruction can be executed correctly
TEST_F(MachineCodeControllerTest, SingleInstruction) {
  auto inst = MakeInstructionRegImm(llvm::X86::MOV64ri, llvm::X86::RAX, 1337);
  automat::library::Instruction* instructions[] = {inst.get()};
  TestUpdateCode(instructions);

  StartExecution(inst);
  ASSERT_TRUE(WaitForExecution());
  VerifyState({.regs = {.RAX = 1337}}, ToMC(*inst), automat::mc::CodeType::Next);
}

// Two separate instructions, executed one at a time
TEST_F(MachineCodeControllerTest, TwoSeparateInstructions) {
  auto inst1 = MakeInstructionRegImm(llvm::X86::MOV64ri, llvm::X86::RAX, 1337);
  auto inst2 = MakeInstructionRegImm(llvm::X86::MOV64ri, llvm::X86::RBX, 42);
  automat::library::Instruction* instructions[] = {inst1.get(), inst2.get()};
  TestUpdateCode(instructions);

  StartExecution(inst2);
  ASSERT_TRUE(WaitForExecution());
  VerifyState({.regs = {.RBX = 42}}, ToMC(*inst2), automat::mc::CodeType::Next);

  StartExecution(inst1);
  ASSERT_TRUE(WaitForExecution());
  VerifyState({.regs = {.RAX = 1337, .RBX = 42}}, ToMC(*inst1), automat::mc::CodeType::Next);
}

// Two instructions, executed one after the other
TEST_F(MachineCodeControllerTest, TwoSequentialInstructions) {
  auto inst1 = MakeInstructionRegImm(llvm::X86::MOV64ri, llvm::X86::RAX, 1337);
  auto inst2 = MakeInstructionRegImm(llvm::X86::MOV64ri, llvm::X86::RBX, 42);
  Next(inst1, inst2);
  automat::library::Instruction* instructions[] = {inst1.get(), inst2.get()};
  TestUpdateCode(instructions);

  StartExecution(inst1);
  ASSERT_TRUE(WaitForExecution());
  VerifyState({.regs = {.RAX = 1337, .RBX = 42}}, ToMC(*inst2), automat::mc::CodeType::Next);
}

TEST_F(MachineCodeControllerTest, JumpExitInstruction) {
  // Create a jump instruction not connected to any other instruction.
  // This should trigger a fixup that generates an exit at a Jump exit point.
  auto inst = MakeInstructionImm(llvm::X86::JMP_4, 0);

  automat::library::Instruction* instructions[] = {inst.get()};
  TestUpdateCode(instructions);

  StartExecution(inst);
  ASSERT_TRUE(WaitForExecution());

  // Expect that registers remain zero and that we exit at a Jump exit point.
  VerifyState({.regs = {}}, ToMC(*inst), automat::mc::CodeType::Jump);
}

TEST_F(MachineCodeControllerTest, InfiniteLoop) {
  auto inst = MakeInstructionRegImm(llvm::X86::MOV64ri, llvm::X86::RAX, 42);
  Next(inst, inst);
  automat::library::Instruction* instructions[] = {inst.get()};
  TestUpdateCode(instructions);
  StartExecution(inst);
  ASSERT_FALSE(WaitForExecution(10ms));

  VerifyState({.current_instruction = ToMC(*inst), .regs = {.RAX = 42}});
}

TEST_F(MachineCodeControllerTest, HotReload) {
  auto inst1 = MakeInstructionRegImm(llvm::X86::MOV64ri, llvm::X86::RAX, 1337);
  auto inst2 = MakeInstructionRegImm(llvm::X86::MOV64ri, llvm::X86::RAX, 42);

  // Start executing inst2 in a loop.
  auto conn = Next(inst2, inst2);
  automat::library::Instruction* instructions[] = {inst1.get(), inst2.get()};
  TestUpdateCode(instructions);
  StartExecution(inst2);
  ASSERT_FALSE(WaitForExecution(10ms));
  VerifyState({.current_instruction = ToMC(*inst2), .regs = {.RAX = 42}});

  // Then break the loop by redirecting inst2 to inst1.
  delete conn;
  Next(inst2, inst1);
  automat::library::Instruction* instructions2[] = {inst2.get(), inst1.get()};
  TestUpdateCode(instructions2);
  ASSERT_TRUE(WaitForExecution());
  VerifyState({.regs = {.RAX = 1337}}, ToMC(*inst1), automat::mc::CodeType::Next);
}
