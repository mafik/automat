// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "error_recovery.hpp"

#if defined(__linux__)
#include <pthread.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <windows.h>

#include <cstdlib>
#endif

#include <algorithm>
#include <cstdio>
#include <initializer_list>
#include <mutex>
#include <thread>

#include "gtest.hpp"

using namespace automat;
using ::testing::ExitedWithCode;
#if defined(__linux__)
using ::testing::KilledBySignal;
#endif

namespace {

enum ChildResult : int {
  kRecovered = 0,
  kNoFault = 3,
  kWrongRaii = 4,
  kWrongErrorType = 5,
  kNoSourceLocation = 6,
  kWrongSourceLocation = 7,
  kLockLeaked = 8,
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
  ERROR_RECOVERY_TRY {
    OuterWithRaii();
    _exit(kNoFault);
  }
  ERROR_RECOVERY_CATCH(error) {
    if (error.type != LowLevelError::Type::WROTE_UNMAPPED_MEMORY) _exit(kWrongErrorType);
    auto location = error.FindSourceLocation();
    if (!location) _exit(kNoSourceLocation);
    if (!location->file_name().contains("error_recovery_test.cpp") ||
        !location->function_name().contains("FaultInFrameWithRaii")) {
      fprintf(stderr, "resolved to %s:%u in %s\n", Str(location->file_name()).c_str(),
              location->line, Str(location->function_name()).c_str());
      _exit(kWrongSourceLocation);
    }
    bool wrong_raii = !outer_destroyed || !inner_destroyed;
    if (wrong_raii) {
      fprintf(stderr, "outer_destroyed=%d inner_destroyed=%d\n", outer_destroyed,
              inner_destroyed);
      _exit(kWrongRaii);
    }
    _exit(kRecovered);
  }
  _exit(kNoFault);
}

std::mutex lock_fault_mutex;

__attribute__((noinline)) void FaultUnderLock() {
  std::lock_guard<std::mutex> guard(lock_fault_mutex);
  *(volatile int*)1339 = 42;
  OpaqueSink("unreachable");
}
void (*volatile FaultUnderLockBarrier)() = FaultUnderLock;

[[noreturn]] void CheckFaultUnderLock() {
  ERROR_RECOVERY_TRY {
    FaultUnderLockBarrier();
    _exit(kNoFault);
  }
  ERROR_RECOVERY_CATCH(error) {
    if (error.type != LowLevelError::Type::WROTE_UNMAPPED_MEMORY) _exit(kWrongErrorType);
    if (!lock_fault_mutex.try_lock()) _exit(kLockLeaked);
    lock_fault_mutex.unlock();
    _exit(kRecovered);
  }
  _exit(kNoFault);
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

#if defined(__linux__)
void ReadBelowOwnStack() {
  pthread_attr_t attr;
  pthread_getattr_np(pthread_self(), &attr);
  void* stack_address;
  size_t stack_size;
  pthread_attr_getstack(&attr, &stack_address, &stack_size);
  pthread_attr_destroy(&attr);
  (void)*((volatile char*)stack_address - 1);
}
#elif defined(_WIN32)
void ReadBelowOwnStack() {
  ULONG_PTR low_limit, high_limit;
  GetCurrentThreadStackLimits(&low_limit, &high_limit);
  (void)*(volatile char*)low_limit;
}
#endif

void ReadUnmappedMemory() { (void)*(volatile int*)1339; }

#if defined(__linux__)
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
#elif defined(_WIN32)
void WriteReadOnlyMemory() {
  void* page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READONLY);
  *(volatile char*)page = 1;
}

void ExecNonExecutableMemory() {
  void* page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  ((void (*)())page)();
}

void ExecUnmappedMemory() {
  void* page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  VirtualFree(page, 0, MEM_RELEASE);
  ((void (*)())page)();
}
#endif

void DivideByZero() {
  volatile int zero = 0;
  volatile int one = 1;
  volatile int r = one / zero;
  (void)r;
}

[[noreturn]] void RunScenario(void (*scenario)(),
                              std::initializer_list<LowLevelError::Type> expected_types) {
  void (*volatile barrier)() = scenario;
  ERROR_RECOVERY_TRY {
    barrier();
    _exit(kNoFault);
  }
  ERROR_RECOVERY_CATCH(error) {
    if (!std::ranges::contains(expected_types, error.type)) _exit(kWrongErrorType);
    _exit(kRecovered);
  }
  _exit(kNoFault);
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

bool OverflowIsRecovered() {
  void (*volatile barrier)() = [] { Recurse(nullptr); };
  ERROR_RECOVERY_TRY {
    barrier();
  }
  ERROR_RECOVERY_CATCH(error) {
    return error.type == LowLevelError::Type::STACK_OVERFLOW;
  }
  return false;
}

[[noreturn]] void CheckStackOverflowTwice() {
  if (!OverflowIsRecovered()) _exit(kWrongErrorType);
  if (!OverflowIsRecovered()) _exit(kWrongErrorType);
  _exit(kRecovered);
}

struct SuiteSetup : testing::Environment {
  error_recovery::SignalStack signal_stack;  // scenarios run on the death test child's main thread
  void SetUp() override {
#if defined(__linux__)
    rlimit no_core = {0, 0};  // dying children would otherwise dump the warmed-up debug info
    setrlimit(RLIMIT_CORE, &no_core);
#elif defined(_WIN32)
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    GTEST_FLAG_SET(catch_exceptions, false);
#endif
    error_recovery::Init();
  }
};
auto* const suite_setup = testing::AddGlobalTestEnvironment(new SuiteSetup);

}  // namespace

TEST(ErrorRecoveryTest, RaiiDestruction) {
  EXPECT_EXIT(CheckRaiiDestruction(), ExitedWithCode(kRecovered), "")
      << "A recovered fault destroys the RAII above the barrier and the RAII of the faulting "
         "frame itself, which both platforms reach through the cleanup recorded for the next "
         "call site.";
}

TEST(ErrorRecoveryTest, FaultUnderLockReleasesTheLock) {
  EXPECT_EXIT(CheckFaultUnderLock(), ExitedWithCode(kRecovered), "")
      << "The mutex is locked by a call before the fault, so unwinding must run the guard's "
         "unlock; a leaked lock would deadlock whoever later waits for it.";
}

#if defined(__linux__)
TEST(ErrorRecoveryTest, DtorFaultingDuringUnwindIsRecovered) {
  EXPECT_EXIT(RunScenario(FrameWithFaultingDtor, LowLevelError::Type::WROTE_UNMAPPED_MEMORY),
              ExitedWithCode(kRecovered), "")
      << "A destructor faulting during unwinding abandons the original exception and throws a "
         "new one, which reaches the original catch.";
}
#elif defined(_WIN32)
TEST(ErrorRecoveryTest, DtorFaultingDuringUnwindIsFatal) {
  EXPECT_EXIT(RunScenario(FrameWithFaultingDtor, LowLevelError::Type::WROTE_UNMAPPED_MEMORY),
              ExitedWithCode((int)EXCEPTION_ACCESS_VIOLATION), "")
      << "A fault while an exception is unwinding is not intercepted, because a second throw "
         "during unwinding would terminate the process anyway; the fault kills the process "
         "directly instead.";
}
#endif

TEST(ErrorRecoveryTest, StackOverflowIsRecovered) {
  EXPECT_EXIT(RunScenario([] { Recurse(nullptr); }, LowLevelError::Type::STACK_OVERFLOW),
              ExitedWithCode(kRecovered), "")
      << "The handler runs on memory reserved ahead of time and throws, so unwinding reaches "
         "the catch and the fault is reported as a stack overflow.";
}

TEST(ErrorRecoveryTest, StackOverflowOnWorkerThreadIsRecovered) {
  EXPECT_EXIT(RunScenarioOnWorkerThread([] { Recurse(nullptr); },
                                        LowLevelError::Type::STACK_OVERFLOW),
              ExitedWithCode(kRecovered), "")
      << "A worker thread overflows into the guard region of its own stack, which must still be "
         "reported as a stack overflow.";
}

TEST(ErrorRecoveryTest, StackOverflowTwiceIsRecovered) {
  EXPECT_EXIT(CheckStackOverflowTwice(), ExitedWithCode(kRecovered), "")
      << "The destructor of the caught error restores the guard region consumed by the first "
         "overflow, so the next overflow is reported again instead of killing the process.";
}

TEST(ErrorRecoveryTest, AccessBelowStackIsNotStackOverflow) {
  EXPECT_EXIT(RunScenarioOnWorkerThread(ReadBelowOwnStack,
                                        {LowLevelError::Type::READ_PROTECTED_MEMORY,
                                         LowLevelError::Type::READ_UNMAPPED_MEMORY}),
              ExitedWithCode(kRecovered), "")
      << "A wild pointer into the region guarding the stack is not a stack overflow. The region "
         "is reported as protected or unmapped memory, depending on how the platform installs "
         "it.";
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
         "at the top of the stack leads the unwinder back into unwindable code.";
}

TEST(ErrorRecoveryTest, ExecutingUnmappedMemoryIsRecovered) {
  EXPECT_EXIT(RunScenario(ExecUnmappedMemory, LowLevelError::Type::EXECUTED_UNMAPPED_MEMORY),
              ExitedWithCode(kRecovered), "")
      << "A call into an unmapped page is reported as an execution of unmapped memory and needs "
         "the same return-address treatment as an execution of protected memory.";
}

#if defined(__linux__)
TEST(ErrorRecoveryTest, SigbusIsRecovered) {
  EXPECT_EXIT(RunScenario(TouchTruncatedMapping, LowLevelError::Type::ACCESSED_UNALIGNED_MEMORY),
              ExitedWithCode(kRecovered), "")
      << "Touching a mapping without backing storage raises SIGBUS, which the handler converts "
         "into a LowLevelError.";
}
#endif

TEST(ErrorRecoveryTest, UnknownInstructionIsRecovered) {
  EXPECT_EXIT(RunScenario([] { __builtin_trap(); },
                          LowLevelError::Type::EXECUTED_UNKNOWN_INSTRUCTION),
              ExitedWithCode(kRecovered), "")
      << "__builtin_trap emits an instruction that the CPU refuses to execute, which is reported "
         "as an unknown instruction.";
}

TEST(ErrorRecoveryTest, ArithmeticErrorIsRecovered) {
  EXPECT_EXIT(RunScenario(DivideByZero, LowLevelError::Type::ARITHMETIC_ERROR),
              ExitedWithCode(kRecovered), "")
      << "Integer division by zero is reported as an arithmetic error.";
}

#if defined(__linux__)
TEST(ErrorRecoveryTest, StopMakesFaultsFatalAgain) {
  EXPECT_EXIT(
      {
        error_recovery::Stop();
        RunScenario(FaultLeaf, LowLevelError::Type::WROTE_UNMAPPED_MEMORY);
      },
      KilledBySignal(SIGSEGV), "")
      << "Stop() restores the previous signal dispositions.";
}
#elif defined(_WIN32)
TEST(ErrorRecoveryTest, StopMakesFaultsFatalAgain) {
  EXPECT_EXIT(
      {
        error_recovery::Stop();
        RunScenario(FaultLeaf, LowLevelError::Type::WROTE_UNMAPPED_MEMORY);
      },
      ExitedWithCode((int)EXCEPTION_ACCESS_VIOLATION), "")
      << "Stop() removes the vectored exception handler.";
}
#endif
