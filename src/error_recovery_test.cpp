// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "error_recovery.hpp"

#include <pthread.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <initializer_list>
#include <thread>

#include "gtest.hpp"

using namespace automat;
using ::testing::ExitedWithCode;
using ::testing::KilledBySignal;

namespace {

enum ChildResult : int {
  kRecovered = 0,
  kNoFault = 3,
  kWrongRaii = 4,
  kWrongErrorType = 5,
  kWrongSourceLocation = 6,
};

bool outer_destroyed;
bool inner_destroyed;

void OpaqueSinkImpl(const char*) {}
void (*volatile OpaqueSink)(const char*) = OpaqueSinkImpl;

struct DtorFlag {
  bool* flag;
  DtorFlag(bool* flag) : flag(flag) {}
  ~DtorFlag() { *flag = true; }
};

struct DtorFlagWithCall {
  bool* flag;
  DtorFlagWithCall(bool* flag) : flag(flag) { OpaqueSink("ctor"); }
  ~DtorFlagWithCall() {
    *flag = true;
    OpaqueSink("dtor");
  }
};

__attribute__((noinline)) void FaultInFrameWithRaii() {
  DtorFlagWithCall flag(&inner_destroyed);
  *(volatile int*)1339 = 42;
  OpaqueSink("unreachable");
}
void (*volatile FaultBarrier)() = FaultInFrameWithRaii;

__attribute__((noinline)) void OuterWithRaii() {
  DtorFlag flag(&outer_destroyed);
  FaultBarrier();
}

[[noreturn]] void CheckRaiiDestruction() {
  try {
    OuterWithRaii();
    _exit(kNoFault);
  } catch (LowLevelError& error) {
    if (error.type != LowLevelError::Type::WROTE_UNMAPPED_MEMORY) _exit(kWrongErrorType);
    auto location = error.FindSourceLocation();
    if (!location || !location->file_name().contains("error_recovery_test.cpp") ||
        !location->function_name().contains("FaultInFrameWithRaii")) {
      _exit(kWrongSourceLocation);
    }
    if (!outer_destroyed || inner_destroyed) {
      _exit(kWrongRaii);
    }
    _exit(kRecovered);
  }
}

__attribute__((noinline)) void FaultLeaf() { *(volatile int*)1339 = 42; }
void (*volatile FaultLeafBarrier)() = FaultLeaf;

struct FaultingDtor {
  ~FaultingDtor() { *(volatile int*)1341 = 1; }
};

__attribute__((noinline)) void FrameWithFaultingDtor() {
  FaultingDtor faulting_dtor;
  FaultLeafBarrier();
}

volatile bool never_stop = true;

__attribute__((noinline)) int Recurse(volatile char* prev) {
  volatile char buf[1024];
  buf[0] = prev ? prev[0] + 1 : 0;
  if (!never_stop) return buf[0];
  int r = Recurse(buf);
  return r + buf[0];
}

void ReadBelowOwnStack() {
  pthread_attr_t attr;
  pthread_getattr_np(pthread_self(), &attr);
  void* stack_address;
  size_t stack_size;
  pthread_attr_getstack(&attr, &stack_address, &stack_size);
  pthread_attr_destroy(&attr);
  (void)*((volatile char*)stack_address - 1);
}

void ReadUnmappedMemory() { (void)*(volatile int*)1339; }

void WriteReadOnlyMemory() {
  void* page = mmap(nullptr, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  *(volatile char*)page = 1;
}

void ExecNonExecutableMemory() {
  void* page = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  ((void (*)())page)();
}

void ExecUnmappedMemory() {
  void* page = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  munmap(page, 4096);
  ((void (*)())page)();
}

void TouchTruncatedMapping() {
  int fd = memfd_create("error_recovery_test_sigbus", 0);
  ftruncate(fd, 4096);
  char* p = (char*)mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  ftruncate(fd, 0);
  *(volatile char*)p = 1;
}

void DivideByZero() {
  volatile int zero = 0;
  volatile int one = 1;
  volatile int r = one / zero;
  (void)r;
}

[[noreturn]] void RunScenario(void (*scenario)(),
                              std::initializer_list<LowLevelError::Type> expected_types) {
  void (*volatile barrier)() = scenario;
  try {
    barrier();
    _exit(kNoFault);
  } catch (LowLevelError& error) {
    if (!std::ranges::contains(expected_types, error.type)) _exit(kWrongErrorType);
    _exit(kRecovered);
  }
}

[[noreturn]] void RunScenario(void (*scenario)(), LowLevelError::Type expected_type) {
  RunScenario(scenario, {expected_type});
}

[[noreturn]] void RunScenarioOnWorkerThread(
    void (*scenario)(), std::initializer_list<LowLevelError::Type> expected_types) {
  std::thread([=] {
    error_recovery::SignalStack signal_stack;
    RunScenario(scenario, expected_types);
  }).join();
  _exit(kNoFault);
}

[[noreturn]] void RunScenarioOnWorkerThread(void (*scenario)(), LowLevelError::Type expected_type) {
  RunScenarioOnWorkerThread(scenario, {expected_type});
}

struct SuiteSetup : testing::Environment {
  error_recovery::SignalStack signal_stack;  // scenarios run on the (forked) main thread
  void SetUp() override {
    rlimit no_core = {0, 0};  // dying children would otherwise dump the warmed-up debug info
    setrlimit(RLIMIT_CORE, &no_core);
    error_recovery::Init();
  }
};
auto* const suite_setup = testing::AddGlobalTestEnvironment(new SuiteSetup);

}  // namespace

TEST(ErrorRecoveryTest, RaiiDestruction) {
  EXPECT_EXIT(CheckRaiiDestruction(), ExitedWithCode(kRecovered), "")
      << "A recovered fault should destroy the RAII above the barrier and skip the RAII in the "
         "faulting frame. If the skipped one is now destroyed, clang may have implemented "
         "-fnon-call-exceptions.";
}

TEST(ErrorRecoveryTest, DtorFaultingDuringUnwindIsRecovered) {
  EXPECT_EXIT(RunScenario(FrameWithFaultingDtor, LowLevelError::Type::WROTE_UNMAPPED_MEMORY),
              ExitedWithCode(kRecovered), "")
      << "A destructor faulting during unwinding abandons the original exception and throws a "
         "new one, which reaches the original catch.";
}

TEST(ErrorRecoveryTest, StackOverflowIsRecovered) {
  EXPECT_EXIT(RunScenario([] { Recurse(nullptr); }, LowLevelError::Type::STACK_OVERFLOW),
              ExitedWithCode(kRecovered), "")
      << "The handler runs on the signal stack and throws through the signal frame, so unwinding "
         "resumes in the faulting frame. The fault lies within a page of the stack pointer, "
         "which classifies it as a stack overflow.";
}

TEST(ErrorRecoveryTest, StackOverflowOnWorkerThreadIsRecovered) {
  EXPECT_EXIT(RunScenarioOnWorkerThread([] { Recurse(nullptr); },
                                        LowLevelError::Type::STACK_OVERFLOW),
              ExitedWithCode(kRecovered), "")
      << "A worker thread overflows into the guard page of its pthread stack, a protected "
         "mapping, which must still be reported as a stack overflow.";
}

TEST(ErrorRecoveryTest, AccessBelowStackIsNotStackOverflow) {
  EXPECT_EXIT(RunScenarioOnWorkerThread(ReadBelowOwnStack,
                                        {LowLevelError::Type::READ_PROTECTED_MEMORY,
                                         LowLevelError::Type::READ_UNMAPPED_MEMORY}),
              ExitedWithCode(kRecovered), "")
      << "A wild pointer into the guard region is not a stack overflow while the stack pointer "
         "is far away from it. The guard region is a protected mapping, or, when glibc installs "
         "it with MADV_GUARD_INSTALL, a region that the kernel reports as unmapped.";
}

TEST(ErrorRecoveryTest, ReadingUnmappedMemoryIsRecovered) {
  EXPECT_EXIT(RunScenario(ReadUnmappedMemory, LowLevelError::Type::READ_UNMAPPED_MEMORY),
              ExitedWithCode(kRecovered), "")
      << "A read of an unmapped address is reported as a read, not as a write.";
}

TEST(ErrorRecoveryTest, WritingReadOnlyMemoryIsRecovered) {
  EXPECT_EXIT(RunScenario(WriteReadOnlyMemory, LowLevelError::Type::WROTE_PROTECTED_MEMORY),
              ExitedWithCode(kRecovered), "")
      << "A write to a read-only mapping is reported as a write to protected memory.";
}

TEST(ErrorRecoveryTest, ExecutingNonExecutableMemoryIsRecovered) {
  EXPECT_EXIT(RunScenario(ExecNonExecutableMemory, LowLevelError::Type::EXECUTED_PROTECTED_MEMORY),
              ExitedWithCode(kRecovered), "")
      << "The faulting address has no unwind info, but the return address the faulting call left "
         "at [RSP] leads the unwinder back into unwindable code.";
}

TEST(ErrorRecoveryTest, ExecutingUnmappedMemoryIsRecovered) {
  EXPECT_EXIT(RunScenario(ExecUnmappedMemory, LowLevelError::Type::EXECUTED_UNMAPPED_MEMORY),
              ExitedWithCode(kRecovered), "")
      << "A call into an unmapped page is reported as an execution of unmapped memory and needs "
         "the same return-address fixup as an execution of protected memory.";
}

TEST(ErrorRecoveryTest, SigbusIsRecovered) {
  EXPECT_EXIT(RunScenario(TouchTruncatedMapping, LowLevelError::Type::ACCESSED_UNALIGNED_MEMORY),
              ExitedWithCode(kRecovered), "")
      << "Touching a mapping without backing storage raises SIGBUS, which the handler converts "
         "into a LowLevelError.";
}

TEST(ErrorRecoveryTest, SigillIsRecovered) {
  EXPECT_EXIT(RunScenario([] { __builtin_trap(); },
                          LowLevelError::Type::EXECUTED_UNKNOWN_INSTRUCTION),
              ExitedWithCode(kRecovered), "")
      << "The trap instruction raises SIGILL, which the handler converts into a LowLevelError.";
}

TEST(ErrorRecoveryTest, SigfpeIsRecovered) {
  EXPECT_EXIT(RunScenario(DivideByZero, LowLevelError::Type::ARITHMETIC_ERROR),
              ExitedWithCode(kRecovered), "")
      << "Integer division by zero raises SIGFPE, which the handler converts into a "
         "LowLevelError.";
}

TEST(ErrorRecoveryTest, StopMakesFaultsFatalAgain) {
  EXPECT_EXIT(
      {
        error_recovery::Stop();
        RunScenario(FaultLeaf, LowLevelError::Type::WROTE_UNMAPPED_MEMORY);
      },
      KilledBySignal(SIGSEGV), "")
      << "Stop() restores the previous signal dispositions.";
}
