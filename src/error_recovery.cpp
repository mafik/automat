// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
#include "error_recovery.hpp"

#include <llvm/DebugInfo/Symbolize/Symbolize.h>
#include <pthread.h>
#include <signal.h>
#include <ucontext.h>
#include <unwind.h>

#include <cstdint>
#include <iterator>
#include <mutex>

#include "format.hpp"
#include "log.hpp"

#pragma maf add link argument "-Wl,--wrap=__gxx_personality_v0"

namespace automat {

namespace {

constexpr bool kDebugErrorRecovery = false;

bool initialized;
constexpr int kSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE};
struct sigaction old_actions[std::size(kSignals)];

std::mutex symbolizer_mutex;
llvm::symbolize::LLVMSymbolizer symbolizer;

constexpr uintptr_t kStackOverflowReach = 4096;

bool IsStackOverflow(siginfo_t* si, ucontext_t* context) {
  auto fault_address = (uintptr_t)si->si_addr;
  auto stack_pointer = (uintptr_t)context->uc_mcontext.gregs[REG_RSP];
  return fault_address >= stack_pointer - kStackOverflowReach &&
         fault_address < stack_pointer + kStackOverflowReach;
}

LowLevelError::Type ClassifySignal(int sig, siginfo_t* si, ucontext_t* context) {
  using enum LowLevelError::Type;
  switch (sig) {
    case SIGILL:
      return EXECUTED_UNKNOWN_INSTRUCTION;
    case SIGBUS:
      return ACCESSED_UNALIGNED_MEMORY;
    case SIGFPE:
      return ARITHMETIC_ERROR;
    default: {
      auto err = context->uc_mcontext.gregs[REG_ERR];
      bool protected_memory = si->si_code == SEGV_ACCERR || si->si_code == SEGV_PKUERR;
      if (err & 0x10)
        return protected_memory ? EXECUTED_PROTECTED_MEMORY : EXECUTED_UNMAPPED_MEMORY;
      if (IsStackOverflow(si, context)) return STACK_OVERFLOW;
      if (err & 0x2) return protected_memory ? WROTE_PROTECTED_MEMORY : WROTE_UNMAPPED_MEMORY;
      return protected_memory ? READ_PROTECTED_MEMORY : READ_UNMAPPED_MEMORY;
    }
  }
}

void ErrorRecoverySignalHandler(int sig, siginfo_t* si, ucontext_t* context) {
  auto& regs = context->uc_mcontext.gregs;

  if constexpr (kDebugErrorRecovery) {
    LOG << "Signal " << sig;
    LOG << "siginfo:";
    {
      LOG_IndentGuard indent;
      LOG << dump_struct(*si);
    }
    LOG << "ucontext:";
    {
      LOG_IndentGuard indent;
      LOG << dump_struct(*context);
    }
  }

  LowLevelError error{ClassifySignal(sig, si, context), (intptr_t)regs[REG_RIP]};

  using enum LowLevelError::Type;
  if (error.type == EXECUTED_PROTECTED_MEMORY || error.type == EXECUTED_UNMAPPED_MEMORY) {
    regs[REG_RIP] = *(uint64_t*)regs[REG_RSP] - 1;
    regs[REG_RSP] += 8;
  }

  sigset_t unblock;
  sigemptyset(&unblock);
  sigaddset(&unblock, sig);
  pthread_sigmask(SIG_UNBLOCK, &unblock, nullptr);

  if constexpr (kDebugErrorRecovery) {
    LOG << "ErrorRecoverySignalHandler: walking stack:";
    _Unwind_Backtrace(
        [](_Unwind_Context* ctx, void*) {
          int ip_before;
          uintptr_t ip = _Unwind_GetIPInfo(ctx, &ip_before);
          LOG << f("  unwind frame ip={} cfa={}", ip, _Unwind_GetCFA(ctx));
          return _URC_NO_REASON;
        },
        nullptr);
  }
  throw error;
}

constexpr uint8_t kOmit = 0xff;

uint64_t ReadUleb(const uint8_t*& p) {
  uint64_t result = 0;
  int shift = 0;
  uint8_t byte;
  do {
    byte = *p++;
    result |= (uint64_t)(byte & 0x7f) << shift;
    shift += 7;
  } while (byte & 0x80);
  return result;
}

int64_t ReadSleb(const uint8_t*& p) {
  int64_t result = 0;
  int shift = 0;
  uint8_t byte;
  do {
    byte = *p++;
    result |= (int64_t)(byte & 0x7f) << shift;
    shift += 7;
  } while (byte & 0x80);
  if (shift < 64 && (byte & 0x40)) result |= -(int64_t)1 << shift;
  return result;
}

uint64_t ReadEncoded(const uint8_t*& p, uint8_t encoding) {
  switch (encoding & 0x0f) {
    case 0x00:  // absptr
      p += sizeof(uintptr_t);
      return *(const uintptr_t*)(p - sizeof(uintptr_t));
    case 0x01:  // uleb128
      return ReadUleb(p);
    case 0x02:  // udata2
      p += 2;
      return *(const uint16_t*)(p - 2);
    case 0x03:  // udata4
      p += 4;
      return *(const uint32_t*)(p - 4);
    case 0x04:  // udata8
      p += 8;
      return *(const uint64_t*)(p - 8);
    case 0x09:  // sleb128
      return ReadSleb(p);
    case 0x0a:  // sdata2
      p += 2;
      return *(const int16_t*)(p - 2);
    case 0x0b:  // sdata4
      p += 4;
      return *(const int32_t*)(p - 4);
    case 0x0c:  // sdata8
      p += 8;
      return *(const int64_t*)(p - 8);
    default:  // unknown encoding: caller treats the parse as failed
      return ~0ULL;
  }
}

bool IpCoveredByCallSites(_Unwind_Context* context) {
  auto* p = (const uint8_t*)_Unwind_GetLanguageSpecificData(context);
  if (!p) return true;

  int ip_before = 0;
  uintptr_t ip = _Unwind_GetIPInfo(context, &ip_before);
  if (!ip_before) ip -= 1;
  uintptr_t offset = ip - _Unwind_GetRegionStart(context);

  uint8_t lpstart_encoding = *p++;
  if (lpstart_encoding != kOmit) {
    if (ReadEncoded(p, lpstart_encoding) == ~0ULL) return true;
  }
  uint8_t ttype_encoding = *p++;
  if (ttype_encoding != kOmit) ReadUleb(p);
  uint8_t call_site_encoding = *p++;
  uint64_t call_site_table_length = ReadUleb(p);
  const uint8_t* end = p + call_site_table_length;

  while (p < end) {
    uint64_t start = ReadEncoded(p, call_site_encoding);
    uint64_t length = ReadEncoded(p, call_site_encoding);
    ReadEncoded(p, call_site_encoding);  // landing pad
    ReadUleb(p);                         // action
    if (start == ~0ULL || length == ~0ULL) return true;
    if (offset >= start && offset < start + length) return true;
  }
  return false;
}

}  // namespace

Optional<SourceLocation> LowLevelError::FindSourceLocation() const {
  auto lock = std::lock_guard(symbolizer_mutex);
  llvm::DILineInfo info;
  auto result = symbolizer.symbolizeCode(
      std::string("/proc/self/exe"),
      llvm::object::SectionedAddress{(uint64_t)instruction_pointer,
                                     llvm::object::SectionedAddress::UndefSection});
  if (result) {
    info = *result;
  } else {
    llvm::consumeError(result.takeError());
  }
  bool has_file = info.FileName != llvm::DILineInfo::BadString;
  bool has_function = info.FunctionName != llvm::DILineInfo::BadString;
  if (!has_file && !has_function) {
    return std::nullopt;
  }
  return SourceLocation(has_file ? StrView(info.FileName) : StrView("<unknown file>"),
                        has_function ? Str(info.FunctionName) : f("{:#x}", instruction_pointer),
                        info.Line, info.Column);
}

extern "C" _Unwind_Reason_Code __real___gxx_personality_v0(int, _Unwind_Action, uint64_t,
                                                           _Unwind_Exception*, _Unwind_Context*);

extern "C" _Unwind_Reason_Code __wrap___gxx_personality_v0(int version, _Unwind_Action actions,
                                                           uint64_t exception_class,
                                                           _Unwind_Exception* unwind_exception,
                                                           _Unwind_Context* context) {
  if (IpCoveredByCallSites(context)) {
    return __real___gxx_personality_v0(version, actions, exception_class, unwind_exception,
                                       context);
  }
  return _URC_CONTINUE_UNWIND;
}

namespace error_recovery {

SignalStack::SignalStack() {
  stack_t ss = {.ss_sp = stack, .ss_flags = 0, .ss_size = sizeof(stack)};
  sigaltstack(&ss, nullptr);
}

SignalStack::~SignalStack() {
  stack_t disable = {.ss_flags = SS_DISABLE};
  sigaltstack(&disable, nullptr);
}

void Init() {
  if (initialized) return;

  if (auto module = symbolizer.getOrCreateModuleInfo("/proc/self/exe"); !module) {
    llvm::consumeError(module.takeError());
  }

  struct sigaction sa = {};
  sa.sa_sigaction = (void (*)(int, siginfo_t*, void*))ErrorRecoverySignalHandler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  for (size_t i = 0; i < std::size(kSignals); ++i) {
    if (sigaction(kSignals[i], &sa, &old_actions[i]) == -1) {
      ERROR << f("Couldn't install the recovery handler for signal {}", kSignals[i]);
    }
  }

  initialized = true;
}

void Stop() {
  if (!initialized) return;
  for (size_t i = 0; i < std::size(kSignals); ++i) {
    sigaction(kSignals[i], &old_actions[i], nullptr);
  }
  initialized = false;
}

}  // namespace error_recovery

}  // namespace automat
