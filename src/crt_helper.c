// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include <dlfcn.h>
#include <pthread.h>
#include <unwind.h>

__asm__(".symver dlopen,dlopen@GLIBC_2.2.5");
__asm__(".symver dlsym,dlsym@GLIBC_2.2.5");
__asm__(".symver dlclose,dlclose@GLIBC_2.2.5");
__asm__(".symver dlerror,dlerror@GLIBC_2.2.5");
__asm__(".symver pthread_once,pthread_once@GLIBC_2.2.5");
__asm__(".symver __libc_start_main,__libc_start_main@GLIBC_2.2.5");

int __libc_start_main(int (*)(int, char**, char**), int, char**, void (*)(void), void (*)(void),
                      void (*)(void), void*);

struct Unwinder {
  _Unwind_Reason_Code (*raise_exception)(struct _Unwind_Exception*);
  void (*resume)(struct _Unwind_Exception*);
  _Unwind_Reason_Code (*resume_or_rethrow)(struct _Unwind_Exception*);
  void (*delete_exception)(struct _Unwind_Exception*);
  _Unwind_Reason_Code (*forced_unwind)(struct _Unwind_Exception*, _Unwind_Stop_Fn, void*);
  _Unwind_Reason_Code (*backtrace)(_Unwind_Trace_Fn, void*);
  _Unwind_Word (*get_gr)(struct _Unwind_Context*, int);
  void (*set_gr)(struct _Unwind_Context*, int, _Unwind_Word);
  _Unwind_Ptr (*get_ip)(struct _Unwind_Context*);
  _Unwind_Ptr (*get_ip_info)(struct _Unwind_Context*, int*);
  void (*set_ip)(struct _Unwind_Context*, _Unwind_Ptr);
  _Unwind_Word (*get_cfa)(struct _Unwind_Context*);
  void* (*get_language_specific_data)(struct _Unwind_Context*);
  _Unwind_Ptr (*get_region_start)(struct _Unwind_Context*);
  _Unwind_Ptr (*get_data_rel_base)(struct _Unwind_Context*);
  _Unwind_Ptr (*get_text_rel_base)(struct _Unwind_Context*);
  void* (*find_enclosing_function)(void*);
};

static struct Unwinder automat_unwinder;
static const void* automat_cursor_type;
static struct Unwinder libgcc_unwinder;
static pthread_once_t libgcc_unwinder_once = PTHREAD_ONCE_INIT;

static void FindLibgccUnwinder(void) {
  void* libgcc = dlopen("libgcc_s.so.1", RTLD_NOW);
  struct Unwinder* u = &libgcc_unwinder;
  u->resume = dlsym(libgcc, "_Unwind_Resume");
  u->resume_or_rethrow = dlsym(libgcc, "_Unwind_Resume_or_Rethrow");
  u->forced_unwind = dlsym(libgcc, "_Unwind_ForcedUnwind");
  u->get_gr = dlsym(libgcc, "_Unwind_GetGR");
  u->set_gr = dlsym(libgcc, "_Unwind_SetGR");
  u->get_ip = dlsym(libgcc, "_Unwind_GetIP");
  u->get_ip_info = dlsym(libgcc, "_Unwind_GetIPInfo");
  u->set_ip = dlsym(libgcc, "_Unwind_SetIP");
  u->get_cfa = dlsym(libgcc, "_Unwind_GetCFA");
  u->get_language_specific_data = dlsym(libgcc, "_Unwind_GetLanguageSpecificData");
  u->get_region_start = dlsym(libgcc, "_Unwind_GetRegionStart");
  u->get_data_rel_base = dlsym(libgcc, "_Unwind_GetDataRelBase");
  u->get_text_rel_base = dlsym(libgcc, "_Unwind_GetTextRelBase");
}

static struct Unwinder* LibgccUnwinder(void) {
  pthread_once(&libgcc_unwinder_once, FindLibgccUnwinder);
  return &libgcc_unwinder;
}

static struct Unwinder* UnwinderOfContext(struct _Unwind_Context* context) {
  return *(const void**)context == automat_cursor_type ? &automat_unwinder : LibgccUnwinder();
}

static struct Unwinder* UnwinderOfException(struct _Unwind_Exception* exception) {
  return exception->private_1 ? LibgccUnwinder() : &automat_unwinder;
}

_Unwind_Reason_Code _Unwind_RaiseException(struct _Unwind_Exception* exception) {
  __attribute__((musttail)) return automat_unwinder.raise_exception(exception);
}
void _Unwind_Resume(struct _Unwind_Exception* exception) {
  __attribute__((musttail)) return UnwinderOfException(exception)->resume(exception);
}
_Unwind_Reason_Code _Unwind_Resume_or_Rethrow(struct _Unwind_Exception* exception) {
  __attribute__((musttail)) return UnwinderOfException(exception)->resume_or_rethrow(exception);
}
void _Unwind_DeleteException(struct _Unwind_Exception* exception) {
  automat_unwinder.delete_exception(exception);
}
_Unwind_Reason_Code _Unwind_ForcedUnwind(struct _Unwind_Exception* exception, _Unwind_Stop_Fn stop,
                                         void* argument) {
  __attribute__((musttail)) return LibgccUnwinder()->forced_unwind(exception, stop, argument);
}
_Unwind_Reason_Code _Unwind_Backtrace(_Unwind_Trace_Fn trace, void* argument) {
  __attribute__((musttail)) return automat_unwinder.backtrace(trace, argument);
}
_Unwind_Word _Unwind_GetGR(struct _Unwind_Context* context, int index) {
  return UnwinderOfContext(context)->get_gr(context, index);
}
void _Unwind_SetGR(struct _Unwind_Context* context, int index, _Unwind_Word value) {
  UnwinderOfContext(context)->set_gr(context, index, value);
}
_Unwind_Ptr _Unwind_GetIP(struct _Unwind_Context* context) {
  return UnwinderOfContext(context)->get_ip(context);
}
_Unwind_Ptr _Unwind_GetIPInfo(struct _Unwind_Context* context, int* before_instruction) {
  return UnwinderOfContext(context)->get_ip_info(context, before_instruction);
}
void _Unwind_SetIP(struct _Unwind_Context* context, _Unwind_Ptr value) {
  UnwinderOfContext(context)->set_ip(context, value);
}
_Unwind_Word _Unwind_GetCFA(struct _Unwind_Context* context) {
  return UnwinderOfContext(context)->get_cfa(context);
}
void* _Unwind_GetLanguageSpecificData(struct _Unwind_Context* context) {
  return UnwinderOfContext(context)->get_language_specific_data(context);
}
_Unwind_Ptr _Unwind_GetRegionStart(struct _Unwind_Context* context) {
  return UnwinderOfContext(context)->get_region_start(context);
}
_Unwind_Ptr _Unwind_GetDataRelBase(struct _Unwind_Context* context) {
  return UnwinderOfContext(context)->get_data_rel_base(context);
}
_Unwind_Ptr _Unwind_GetTextRelBase(struct _Unwind_Context* context) {
  return UnwinderOfContext(context)->get_text_rel_base(context);
}
void* _Unwind_FindEnclosingFunction(void* address) {
  return automat_unwinder.find_enclosing_function(address);
}

int HelperMain(int argc, char** argv, char** envp) {
  if (argc != 2) {
    return 1;
  }
  long address = 0;
  for (const char* digit = argv[1]; *digit; ++digit) {
    if (*digit < '0' || *digit > '9') {
      return 2;
    }
    address = address * 10 + (*digit - '0');
  }
  void* functions[] = {dlopen, dlsym, dlclose, dlerror, &automat_unwinder, &automat_cursor_type};
  return ((int (*)(void*))address)(functions);
}

__asm__(
    ".text\n"
    ".globl _start\n"
    ".type _start,@function\n"
    "_start:\n"
    "  xor %ebp, %ebp\n"
    "  mov %rdx, %r9\n"
    "  pop %rsi\n"
    "  mov %rsp, %rdx\n"
    "  and $-16, %rsp\n"
    "  push %rax\n"
    "  push %rsp\n"
    "  xor %r8d, %r8d\n"
    "  xor %ecx, %ecx\n"
    "  lea HelperMain(%rip), %rdi\n"
    "  call *__libc_start_main@GOTPCREL(%rip)\n"
    "  hlt\n");
