// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
#include "error_recovery.hpp"

#include <llvm/DebugInfo/Symbolize/Symbolize.h>

#include <cstdint>
#include <mutex>

#include "format.hpp"
#include "log.hpp"

#if defined(__linux__)

#include <pthread.h>
#include <signal.h>
#include <ucontext.h>
#include <unwind.h>

#include <iterator>

#pragma maf add link argument "-Wl,--wrap=__gxx_personality_v0"

#elif defined(_WIN32)

#include <malloc.h>

#include <atomic>
#include <exception>

#include "win32.hpp"

extern "C" void __CxxFrameHandler3();

#endif

namespace automat {

namespace {

bool initialized;
std::mutex symbolizer_mutex;
llvm::symbolize::LLVMSymbolizer symbolizer;
Str exe_path;

#if defined(__linux__)

constexpr bool kDebugErrorRecovery = false;

constexpr int kSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE};
struct sigaction old_actions[std::size(kSignals)];

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

constexpr uint8_t kOmit = 0xff;

[[gnu::always_inline]] inline uint64_t ReadUleb(const uint8_t*& p) {
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

[[gnu::always_inline]] inline int64_t ReadSleb(const uint8_t*& p) {
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

[[gnu::always_inline]] inline uint64_t ReadEncoded(const uint8_t*& p, uint8_t encoding) {
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

[[gnu::always_inline]] inline bool ChainHasCleanup(const uint8_t* action_table, uint64_t action) {
  const uint8_t* record = action_table + (action - 1);
  while (true) {
    int64_t ttype_index = ReadSleb(record);
    if (ttype_index == 0) return true;
    const uint8_t* offset_field = record;
    int64_t next = ReadSleb(record);
    if (next == 0) return false;
    record = offset_field + next;
  }
}

struct CallSite {
  uint64_t start;
  uint64_t length;
  uint64_t landing_pad;
  uint64_t action;
};

struct CallSiteTable {
  const uint8_t* p;
  const uint8_t* end = nullptr;
  const uint8_t* action_table = nullptr;
  uint8_t call_site_encoding = kOmit;
  bool broken = false;

  [[gnu::always_inline]] inline CallSiteTable(const uint8_t* lsda) : p(lsda) {
    if (!p) {
      broken = true;
      return;
    }
    uint8_t lpstart_encoding = *p++;
    if (lpstart_encoding != kOmit && ReadEncoded(p, lpstart_encoding) == ~0ULL) {
      broken = true;
      return;
    }
    uint8_t ttype_encoding = *p++;
    if (ttype_encoding != kOmit) ReadUleb(p);
    call_site_encoding = *p++;
    uint64_t call_site_table_length = ReadUleb(p);
    end = p + call_site_table_length;
    action_table = end;
  }

  [[gnu::always_inline]] inline bool Next(CallSite& out) {
    if (broken || p >= end) return false;
    out.start = ReadEncoded(p, call_site_encoding);
    out.length = ReadEncoded(p, call_site_encoding);
    out.landing_pad = ReadEncoded(p, call_site_encoding);
    out.action = ReadUleb(p);
    if (out.start == ~0ULL || out.length == ~0ULL || out.landing_pad == ~0ULL) {
      broken = true;
      return false;
    }
    return true;
  }
};

struct ReturnAddressSearch {
  uintptr_t fault_ip;
  uintptr_t result;
};

_Unwind_Reason_Code FindFaultCallSite(_Unwind_Context* context, void* arg) {
  auto& search = *(ReturnAddressSearch*)arg;
  int ip_before = 0;
  uintptr_t ip = _Unwind_GetIPInfo(context, &ip_before);
  if (!ip_before || ip != search.fault_ip) return _URC_NO_REASON;
  uintptr_t region_start = _Unwind_GetRegionStart(context);
  uintptr_t offset = ip - region_start;
  CallSiteTable table((const uint8_t*)_Unwind_GetLanguageSpecificData(context));
  CallSite call_site;
  bool covered_with_landing_pad = false;
  uint64_t next_start = ~0ULL;
  while (table.Next(call_site)) {
    if (offset >= call_site.start && offset < call_site.start + call_site.length) {
      if (call_site.landing_pad != 0) covered_with_landing_pad = true;
    } else if (call_site.start > offset && call_site.start < next_start &&
               (call_site.action == 0 || ChainHasCleanup(table.action_table, call_site.action))) {
      next_start = call_site.start;
    }
  }
  if (!table.broken && !covered_with_landing_pad && next_start != ~0ULL) {
    search.result = region_start + (uintptr_t)next_start;
  }
  return _URC_END_OF_STACK;
}

uintptr_t UnwindReturnAddress(uintptr_t fault_ip) {
  ReturnAddressSearch search = {.fault_ip = fault_ip, .result = fault_ip};
  _Unwind_Backtrace(FindFaultCallSite, &search);
  return search.result;
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

  regs[REG_RIP] = UnwindReturnAddress(regs[REG_RIP]);

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

bool IpCoveredByCallSites(_Unwind_Context* context) {
  auto* lsda = (const uint8_t*)_Unwind_GetLanguageSpecificData(context);
  if (!lsda) return true;
  int ip_before = 0;
  uintptr_t ip = _Unwind_GetIPInfo(context, &ip_before);
  if (!ip_before) ip -= 1;
  uintptr_t offset = ip - _Unwind_GetRegionStart(context);
  CallSiteTable table(lsda);
  CallSite call_site;
  while (table.Next(call_site)) {
    if (offset >= call_site.start && offset < call_site.start + call_site.length) return true;
  }
  return table.broken;
}

#elif defined(_WIN32)

void* exception_handler;

std::atomic<DWORD> recovered_threads[64];

bool IsRecoveredThread(DWORD thread_id) {
  for (auto& slot : recovered_threads) {
    if (slot.load(std::memory_order_relaxed) == thread_id) return true;
  }
  return false;
}

LowLevelError::Type Classify(EXCEPTION_RECORD& record) {
  using enum LowLevelError::Type;
  switch (record.ExceptionCode) {
    case EXCEPTION_STACK_OVERFLOW:
      return STACK_OVERFLOW;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
      return EXECUTED_UNKNOWN_INSTRUCTION;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_INT_OVERFLOW:
      return ARITHMETIC_ERROR;
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
      return ACCESSED_UNALIGNED_MEMORY;
    default: {
      bool write = record.ExceptionInformation[0] == 1;
      bool execute = record.ExceptionInformation[0] == 8;
      MEMORY_BASIC_INFORMATION info = {};
      auto* address = (void*)record.ExceptionInformation[1];
      bool protected_memory =
          VirtualQuery(address, &info, sizeof(info)) && info.State == MEM_COMMIT;
      if (execute) return protected_memory ? EXECUTED_PROTECTED_MEMORY : EXECUTED_UNMAPPED_MEMORY;
      if (write) return protected_memory ? WROTE_PROTECTED_MEMORY : WROTE_UNMAPPED_MEMORY;
      return protected_memory ? READ_PROTECTED_MEMORY : READ_UNMAPPED_MEMORY;
    }
  }
}

constexpr uint8_t kUnwindExceptionHandler = 0x01;
constexpr uint8_t kUnwindChainInfo = 0x04;
constexpr uint32_t kFuncInfoMagicMask = 0x1fffffff;
constexpr uint32_t kFuncInfoMagicFirst = 0x19930520;
constexpr uint32_t kFuncInfoMagicLast = 0x19930522;

struct CxxFunctionInfo {
  uint32_t magic;
  int32_t max_state;
  int32_t unwind_map;
  uint32_t try_block_count;
  int32_t try_block_map;
  uint32_t ip_to_state_count;
  int32_t ip_to_state_map;
};

struct IpToStateEntry {
  uint32_t ip;
  int32_t state;
};

const uint8_t* UnwindHandlerData(const uint8_t* unwind_info) {
  return unwind_info + 4 + 2 * ((unwind_info[2] + 1) & ~1);
}

const CxxFunctionInfo* FindCxxFunctionInfo(uintptr_t image_base, RUNTIME_FUNCTION* function) {
  for (int depth = 0; depth < 8; ++depth) {
    auto* unwind_info = (const uint8_t*)(image_base + function->UnwindInfoAddress);
    auto* handler_data = UnwindHandlerData(unwind_info);
    uint8_t flags = unwind_info[0] >> 3;
    if (flags & kUnwindChainInfo) {
      function = (RUNTIME_FUNCTION*)handler_data;
      continue;
    }
    if (!(flags & kUnwindExceptionHandler)) return nullptr;
    auto* rvas = (const uint32_t*)handler_data;
    if (image_base + rvas[0] != (uintptr_t)&__CxxFrameHandler3) return nullptr;
    auto* info = (const CxxFunctionInfo*)(image_base + rvas[1]);
    uint32_t magic = info->magic & kFuncInfoMagicMask;
    if (magic < kFuncInfoMagicFirst || magic > kFuncInfoMagicLast) return nullptr;
    return info;
  }
  return nullptr;
}

uintptr_t UnwindReturnAddress(uintptr_t instruction_pointer) {
  DWORD64 image_base = 0;
  auto* function = RtlLookupFunctionEntry(instruction_pointer, &image_base, nullptr);
  if (!function) return instruction_pointer;
  auto* info = FindCxxFunctionInfo(image_base, function);
  if (!info || info->ip_to_state_count == 0) return instruction_pointer;
  auto* entries = (const IpToStateEntry*)(image_base + info->ip_to_state_map);
  auto fault_rva = (uint32_t)(instruction_pointer - image_base);
  for (uint32_t i = 0; i < info->ip_to_state_count; ++i) {
    if (entries[i].ip <= fault_rva) continue;
    if (entries[i].ip >= function->EndAddress) break;
    return image_base + entries[i].ip;
  }
  return instruction_pointer;
}

[[noreturn]] void ThrowLowLevelError(uintptr_t type, uintptr_t instruction_pointer) {
  alignas(32) volatile char stack_realignment[32];
  stack_realignment[0] = 0;
  throw LowLevelError{(LowLevelError::Type)type, (intptr_t)instruction_pointer};
}

LONG NTAPI ErrorRecoveryExceptionHandler(EXCEPTION_POINTERS* exception) {
  switch (exception->ExceptionRecord->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_INT_OVERFLOW:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
      break;
    default:
      return EXCEPTION_CONTINUE_SEARCH;
  }
  if (!IsRecoveredThread(GetCurrentThreadId())) return EXCEPTION_CONTINUE_SEARCH;
  if (std::uncaught_exceptions() > 0) return EXCEPTION_CONTINUE_SEARCH;
  auto type = Classify(*exception->ExceptionRecord);
  CONTEXT& context = *exception->ContextRecord;
  uintptr_t instruction_pointer = context.Rip;
  context.Rsp -= 8;
  *(uintptr_t*)context.Rsp = UnwindReturnAddress(instruction_pointer);
  context.Rcx = (uintptr_t)type;
  context.Rdx = instruction_pointer;
  context.Rip = (uintptr_t)&ThrowLowLevelError;
  return EXCEPTION_CONTINUE_EXECUTION;
}

#endif

}  // namespace

LowLevelError::~LowLevelError() {
#if defined(_WIN32)
  if (type == Type::STACK_OVERFLOW && !_resetstkoflw()) {
    ERROR << "_resetstkoflw failed, the next stack overflow on this thread will be fatal";
  }
#endif
}

Optional<SourceLocation> LowLevelError::FindSourceLocation() const {
  auto lock = std::lock_guard(symbolizer_mutex);
  if (exe_path.empty()) return std::nullopt;
  llvm::DIInliningInfo inlining_info;
  auto result = symbolizer.symbolizeInlinedCode(
      exe_path, llvm::object::SectionedAddress{(uint64_t)instruction_pointer,
                                               llvm::object::SectionedAddress::UndefSection});
  if (result) {
    inlining_info = *result;
  } else {
    llvm::consumeError(result.takeError());
  }
  if (inlining_info.getNumberOfFrames() == 0) {
    return std::nullopt;
  }
  llvm::DILineInfo info = inlining_info.getFrame(0);
  bool has_file = info.FileName != llvm::DILineInfo::BadString;
  bool has_function = info.FunctionName != llvm::DILineInfo::BadString;
  if (!has_file && !has_function) {
    return std::nullopt;
  }
  return SourceLocation(has_file ? StrView(info.FileName) : StrView("<unknown file>"),
                        has_function ? Str(info.FunctionName) : f("{:#x}", instruction_pointer),
                        info.Line, info.Column);
}

#if defined(__linux__)

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

#endif

namespace error_recovery {

#if defined(__linux__)

SignalStack::SignalStack() {
  stack_t ss = {.ss_sp = stack, .ss_flags = 0, .ss_size = sizeof(stack)};
  sigaltstack(&ss, nullptr);
}

SignalStack::~SignalStack() {
  stack_t disable = {.ss_flags = SS_DISABLE};
  sigaltstack(&disable, nullptr);
}

#elif defined(_WIN32)

SignalStack::SignalStack() {
  ULONG guarantee = 64 * 1024;
  if (!SetThreadStackGuarantee(&guarantee)) {
    ERROR << "SetThreadStackGuarantee: " << win32::GetLastErrorStr();
  }
  DWORD thread_id = GetCurrentThreadId();
  for (auto& slot : recovered_threads) {
    DWORD expected = 0;
    if (slot.compare_exchange_strong(expected, thread_id, std::memory_order_relaxed)) return;
  }
  ERROR << "Error recovery has no free thread slot";
}

SignalStack::~SignalStack() {
  DWORD thread_id = GetCurrentThreadId();
  for (auto& slot : recovered_threads) {
    DWORD expected = thread_id;
    if (slot.compare_exchange_strong(expected, 0, std::memory_order_relaxed)) return;
  }
}

#endif

void Init() {
  if (initialized) return;

#if defined(__linux__)
  exe_path = "/proc/self/exe";
#elif defined(_WIN32)
  wchar_t path[MAX_PATH];
  if (GetModuleFileNameW(nullptr, path, MAX_PATH)) {
    exe_path = win32::WideToUtf8(path);
  }
#endif
  if (!exe_path.empty()) {
    if (auto module = symbolizer.getOrCreateModuleInfo(exe_path); !module) {
      llvm::consumeError(module.takeError());
    }
  }

#if defined(__linux__)
  struct sigaction sa = {};
  sa.sa_sigaction = (void (*)(int, siginfo_t*, void*))ErrorRecoverySignalHandler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  for (size_t i = 0; i < std::size(kSignals); ++i) {
    if (sigaction(kSignals[i], &sa, &old_actions[i]) == -1) {
      ERROR << f("Couldn't install the recovery handler for signal {}", kSignals[i]);
    }
  }
#elif defined(_WIN32)
  exception_handler = AddVectoredExceptionHandler(1, ErrorRecoveryExceptionHandler);
#endif

  initialized = true;
}

void Stop() {
  if (!initialized) return;
#if defined(__linux__)
  for (size_t i = 0; i < std::size(kSignals); ++i) {
    sigaction(kSignals[i], &old_actions[i], nullptr);
  }
#elif defined(_WIN32)
  RemoveVectoredExceptionHandler(exception_handler);
  exception_handler = nullptr;
#endif
  initialized = false;
}

}  // namespace error_recovery

}  // namespace automat
