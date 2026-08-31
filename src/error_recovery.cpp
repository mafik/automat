// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT
#include "error_recovery.hpp"

#ifdef __linux__

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

bool ChainHasCleanup(const uint8_t* action_table, uint64_t action) {
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

struct CallSiteSearch {
  bool defer_to_real;
  uintptr_t cleanup_landing_pad;
};

CallSiteSearch FindCallSiteForIp(_Unwind_Context* context) {
  CallSiteSearch defer = {.defer_to_real = true, .cleanup_landing_pad = 0};
  auto* p = (const uint8_t*)_Unwind_GetLanguageSpecificData(context);
  if (!p) return defer;

  int ip_before = 0;
  uintptr_t ip = _Unwind_GetIPInfo(context, &ip_before);
  if (!ip_before) ip -= 1;
  uintptr_t region_start = _Unwind_GetRegionStart(context);
  uintptr_t offset = ip - region_start;

  uintptr_t landing_pad_base = region_start;
  uint8_t lpstart_encoding = *p++;
  if (lpstart_encoding != kOmit) {
    uint64_t value = ReadEncoded(p, lpstart_encoding);
    if (value == ~0ULL) return defer;
    landing_pad_base = value;
  }
  uint8_t ttype_encoding = *p++;
  if (ttype_encoding != kOmit) ReadUleb(p);
  uint8_t call_site_encoding = *p++;
  uint64_t call_site_table_length = ReadUleb(p);
  const uint8_t* action_table = p + call_site_table_length;

  uint64_t next_start = ~0ULL;
  uint64_t next_landing_pad = 0;
  uint64_t next_action = 0;
  while (p < action_table) {
    uint64_t start = ReadEncoded(p, call_site_encoding);
    uint64_t length = ReadEncoded(p, call_site_encoding);
    uint64_t landing_pad = ReadEncoded(p, call_site_encoding);
    uint64_t action = ReadUleb(p);
    if (start == ~0ULL || length == ~0ULL || landing_pad == ~0ULL) return defer;
    if (offset >= start && offset < start + length) return defer;
    if (start > offset && start < next_start) {
      next_start = start;
      next_landing_pad = landing_pad;
      next_action = action;
    }
  }

  CallSiteSearch uncovered = {.defer_to_real = false, .cleanup_landing_pad = 0};
  if (next_start == ~0ULL || next_landing_pad == 0) return uncovered;
  if (next_action != 0 && !ChainHasCleanup(action_table, next_action)) return uncovered;
  uncovered.cleanup_landing_pad = landing_pad_base + (uintptr_t)next_landing_pad;
  return uncovered;
}

}  // namespace

LowLevelError::~LowLevelError() {}

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
  auto search = FindCallSiteForIp(context);
  if (search.defer_to_real) {
    return __real___gxx_personality_v0(version, actions, exception_class, unwind_exception,
                                       context);
  }
  if ((actions & _UA_CLEANUP_PHASE) && search.cleanup_landing_pad) {
    _Unwind_SetGR(context, __builtin_eh_return_data_regno(0), (uintptr_t)unwind_exception);
    _Unwind_SetGR(context, __builtin_eh_return_data_regno(1), 0);
    _Unwind_SetIP(context, search.cleanup_landing_pad);
    return _URC_INSTALL_CONTEXT;
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

#elif defined(_WIN32)

#include <llvm/DebugInfo/Symbolize/Symbolize.h>
#include <malloc.h>

#include <atomic>
#include <exception>
#include <mutex>

#include "format.hpp"
#include "log.hpp"
#include "win32.hpp"

extern "C" void __CxxFrameHandler3();

namespace automat {

namespace {

bool initialized;
void* exception_handler;

std::mutex symbolizer_mutex;
llvm::symbolize::LLVMSymbolizer symbolizer;
Str exe_path;

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

}  // namespace

LowLevelError::~LowLevelError() {
  if (type == Type::STACK_OVERFLOW && !_resetstkoflw()) {
    ERROR << "_resetstkoflw failed, the next stack overflow on this thread will be fatal";
  }
}

Optional<SourceLocation> LowLevelError::FindSourceLocation() const {
  auto lock = std::lock_guard(symbolizer_mutex);
  if (exe_path.empty()) return std::nullopt;
  llvm::DIInliningInfo inlining_info;
  llvm::DILineInfo info;
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
  info = inlining_info.getFrame(0);
  bool has_file = info.FileName != llvm::DILineInfo::BadString;
  bool has_function = info.FunctionName != llvm::DILineInfo::BadString;
  if (!has_file && !has_function) {
    return std::nullopt;
  }
  return SourceLocation(has_file ? StrView(info.FileName) : StrView("<unknown file>"),
                        has_function ? Str(info.FunctionName) : f("{:#x}", instruction_pointer),
                        info.Line, info.Column);
}

namespace error_recovery {

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

void Init() {
  if (initialized) return;

  wchar_t path[MAX_PATH];
  if (GetModuleFileNameW(nullptr, path, MAX_PATH)) {
    exe_path = win32::WideToUtf8(path);
    if (auto module = symbolizer.getOrCreateModuleInfo(exe_path); !module) {
      llvm::consumeError(module.takeError());
    }
  }
  exception_handler = AddVectoredExceptionHandler(1, ErrorRecoveryExceptionHandler);
  initialized = true;
}

void Stop() {
  if (!initialized) return;
  RemoveVectoredExceptionHandler(exception_handler);
  exception_handler = nullptr;
  initialized = false;
}

}  // namespace error_recovery

}  // namespace automat

#endif
