// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include <dlfcn.h>
#include <link.h>
#include <unwind.h>

__asm__(".symver dlopen,dlopen@GLIBC_2.2.5");
__asm__(".symver dlsym,dlsym@GLIBC_2.2.5");
__asm__(".symver dlclose,dlclose@GLIBC_2.2.5");
__asm__(".symver dlerror,dlerror@GLIBC_2.2.5");
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
  _Unwind_Reason_Code (*personality)(int, _Unwind_Action, _Unwind_Exception_Class,
                                     struct _Unwind_Exception*, struct _Unwind_Context*);
};

static struct Unwinder automat_unwinder;
_Unwind_Reason_Code _Unwind_RaiseException(struct _Unwind_Exception* exception) {
  __attribute__((musttail)) return automat_unwinder.raise_exception(exception);
}
void _Unwind_Resume(struct _Unwind_Exception* exception) {
  __attribute__((musttail)) return automat_unwinder.resume(exception);
}
_Unwind_Reason_Code _Unwind_Resume_or_Rethrow(struct _Unwind_Exception* exception) {
  __attribute__((musttail)) return automat_unwinder.resume_or_rethrow(exception);
}
void _Unwind_DeleteException(struct _Unwind_Exception* exception) {
  automat_unwinder.delete_exception(exception);
}
_Unwind_Reason_Code _Unwind_ForcedUnwind(struct _Unwind_Exception* exception, _Unwind_Stop_Fn stop,
                                         void* argument) {
  __attribute__((musttail)) return automat_unwinder.forced_unwind(exception, stop, argument);
}
_Unwind_Reason_Code _Unwind_Backtrace(_Unwind_Trace_Fn trace, void* argument) {
  __attribute__((musttail)) return automat_unwinder.backtrace(trace, argument);
}
_Unwind_Word _Unwind_GetGR(struct _Unwind_Context* context, int index) {
  return automat_unwinder.get_gr(context, index);
}
void _Unwind_SetGR(struct _Unwind_Context* context, int index, _Unwind_Word value) {
  automat_unwinder.set_gr(context, index, value);
}
_Unwind_Ptr _Unwind_GetIP(struct _Unwind_Context* context) {
  return automat_unwinder.get_ip(context);
}
_Unwind_Ptr _Unwind_GetIPInfo(struct _Unwind_Context* context, int* before_instruction) {
  return automat_unwinder.get_ip_info(context, before_instruction);
}
void _Unwind_SetIP(struct _Unwind_Context* context, _Unwind_Ptr value) {
  automat_unwinder.set_ip(context, value);
}
_Unwind_Word _Unwind_GetCFA(struct _Unwind_Context* context) {
  return automat_unwinder.get_cfa(context);
}
void* _Unwind_GetLanguageSpecificData(struct _Unwind_Context* context) {
  return automat_unwinder.get_language_specific_data(context);
}
_Unwind_Ptr _Unwind_GetRegionStart(struct _Unwind_Context* context) {
  return automat_unwinder.get_region_start(context);
}
_Unwind_Ptr _Unwind_GetDataRelBase(struct _Unwind_Context* context) {
  return automat_unwinder.get_data_rel_base(context);
}
_Unwind_Ptr _Unwind_GetTextRelBase(struct _Unwind_Context* context) {
  return automat_unwinder.get_text_rel_base(context);
}
void* _Unwind_FindEnclosingFunction(void* address) {
  return automat_unwinder.find_enclosing_function(address);
}
_Unwind_Reason_Code __gcc_personality_v0(int version, _Unwind_Action actions,
                                         _Unwind_Exception_Class exception_class,
                                         struct _Unwind_Exception* exception,
                                         struct _Unwind_Context* context) {
  __attribute__((musttail)) return automat_unwinder.personality(version, actions, exception_class,
                                                                exception, context);
}

static const char libgcc_name[] = "libgcc_s.so.1";

static void Override_DT_SONAME(void) {
  dlopen(libgcc_name, RTLD_NOW | RTLD_GLOBAL);
  ElfW(Addr) string_table = 0;
  ElfW(Dyn)* soname = 0;
  for (ElfW(Dyn)* entry = _DYNAMIC; entry->d_tag != DT_NULL; ++entry) {
    if (entry->d_tag == DT_STRTAB) {
      string_table = entry->d_un.d_ptr;
    } else if (entry->d_tag == DT_SONAME) {
      soname = entry;
    }
  }
  soname->d_un.d_val = (ElfW(Addr))libgcc_name - string_table;
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
  Override_DT_SONAME();
  void* functions[] = {dlopen, dlsym, dlclose, dlerror, &automat_unwinder};
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
