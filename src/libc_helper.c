// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

__asm__(".symver dlopen,dlopen@GLIBC_2.2.5");
__asm__(".symver dlsym,dlsym@GLIBC_2.2.5");
__asm__(".symver dlclose,dlclose@GLIBC_2.2.5");
__asm__(".symver dlerror,dlerror@GLIBC_2.2.5");
__asm__(".symver __libc_start_main,__libc_start_main@GLIBC_2.2.5");

void* dlopen(const char*, int);
void* dlsym(void*, const char*);
int dlclose(void*);
char* dlerror(void);
int __libc_start_main(int (*)(int, char**, char**), int, char**, void (*)(void), void (*)(void),
                      void (*)(void), void*);

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
  void* functions[] = {dlopen, dlsym, dlclose, dlerror};
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
