#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Sometimes it's better to call some Linux APIs directly rather than going through libc.

#if defined(__x86_64__)
#define __NR_epoll_create1 291
#define __NR_epoll_ctl 233
#define __NR_epoll_pwait 281
#define __NR_eventfd2 290
#define __NR_timerfd_create 283
#define __NR_timerfd_settime 286
#define __NR_pidfd_open 434
#define __NR_close 3
#define __NR_ptrace 101
#define __NR_mmap 9
#define __NR_mprotect 10
#define __NR_munmap 11
#define __NR_brk 12
#define __NR_mremap 25
#define __NR_madvise 28
#define __NR_fcntl 72
#define __NR_ftruncate 77
#define __NR_fork 57
#define __NR_clone 56
#define __NR_prctl 157
#define __NR_tgkill 234
#define __NR_set_tid_address 218
#define __NR_waitid 247
#define __NR_fallocate 285
#define __NR_prlimit64 302
#define __NR_kcmp 312
#define __NR_memfd_create 319
#define __NR_seccomp 317
#define __NR_pkey_mprotect 329
#define __NR_pidfd_send_signal 424
#define __NR_clone3 435
#define __NR_pidfd_getfd 438
#elif defined(__aarch64__)
#define __NR_epoll_create1 20
#define __NR_epoll_ctl 21
#define __NR_epoll_pwait 22
#define __NR_eventfd2 19
#define __NR_timerfd_create 85
#define __NR_timerfd_settime 86
#define __NR_pidfd_open 434
#define __NR_close 57
#define __NR_ptrace 117
#define __NR_mmap 222
#define __NR_mprotect 226
#define __NR_munmap 215
#define __NR_brk 214
#define __NR_mremap 216
#define __NR_madvise 233
#define __NR_fcntl 25
#define __NR_ftruncate 46
#define __NR_clone 220
#define __NR_prctl 167
#define __NR_tgkill 131
#define __NR_set_tid_address 96
#define __NR_waitid 95
#define __NR_fallocate 47
#define __NR_prlimit64 261
#define __NR_kcmp 272
#define __NR_memfd_create 279
#define __NR_seccomp 277
#define __NR_pkey_mprotect 288
#define __NR_pidfd_send_signal 424
#define __NR_clone3 435
#define __NR_pidfd_getfd 438
#else
#error "LinuxSyscall supports x86-64 and AArch64"
#endif

namespace automat {

namespace {

long LinuxSyscall(long number, long a, long b, long c, long d, long e, long f) {
#if defined(__x86_64__)
  register long rax asm("rax") = number;
  register long rdi asm("rdi") = a;
  register long rsi asm("rsi") = b;
  register long rdx asm("rdx") = c;
  register long r10 asm("r10") = d;
  register long r8 asm("r8") = e;
  register long r9 asm("r9") = f;
  asm volatile("syscall"
               : "+r"(rax)
               : "r"(rdi), "r"(rsi), "r"(rdx), "r"(r10), "r"(r8), "r"(r9)
               : "rcx", "r11", "memory");
  return rax;
#elif defined(__aarch64__)
  register long x8 asm("x8") = number;
  register long x0 asm("x0") = a;
  register long x1 asm("x1") = b;
  register long x2 asm("x2") = c;
  register long x3 asm("x3") = d;
  register long x4 asm("x4") = e;
  register long x5 asm("x5") = f;
  asm volatile("svc #0"
               : "+r"(x0)
               : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
               : "memory");
  return x0;
#endif
}

long LinuxSyscall(long number, long a, long b, long c, long d, long e) {
#if defined(__x86_64__)
  register long rax asm("rax") = number;
  register long rdi asm("rdi") = a;
  register long rsi asm("rsi") = b;
  register long rdx asm("rdx") = c;
  register long r10 asm("r10") = d;
  register long r8 asm("r8") = e;
  asm volatile("syscall"
               : "+r"(rax)
               : "r"(rdi), "r"(rsi), "r"(rdx), "r"(r10), "r"(r8)
               : "rcx", "r11", "memory");
  return rax;
#elif defined(__aarch64__)
  register long x8 asm("x8") = number;
  register long x0 asm("x0") = a;
  register long x1 asm("x1") = b;
  register long x2 asm("x2") = c;
  register long x3 asm("x3") = d;
  register long x4 asm("x4") = e;
  asm volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4) : "memory");
  return x0;
#endif
}

long LinuxSyscall(long number, long a, long b, long c, long d) {
#if defined(__x86_64__)
  register long rax asm("rax") = number;
  register long rdi asm("rdi") = a;
  register long rsi asm("rsi") = b;
  register long rdx asm("rdx") = c;
  register long r10 asm("r10") = d;
  asm volatile("syscall"
               : "+r"(rax)
               : "r"(rdi), "r"(rsi), "r"(rdx), "r"(r10)
               : "rcx", "r11", "memory");
  return rax;
#elif defined(__aarch64__)
  register long x8 asm("x8") = number;
  register long x0 asm("x0") = a;
  register long x1 asm("x1") = b;
  register long x2 asm("x2") = c;
  register long x3 asm("x3") = d;
  asm volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3) : "memory");
  return x0;
#endif
}

long LinuxSyscall(long number, long a, long b, long c) {
#if defined(__x86_64__)
  register long rax asm("rax") = number;
  register long rdi asm("rdi") = a;
  register long rsi asm("rsi") = b;
  register long rdx asm("rdx") = c;
  asm volatile("syscall" : "+r"(rax) : "r"(rdi), "r"(rsi), "r"(rdx) : "rcx", "r11", "memory");
  return rax;
#elif defined(__aarch64__)
  register long x8 asm("x8") = number;
  register long x0 asm("x0") = a;
  register long x1 asm("x1") = b;
  register long x2 asm("x2") = c;
  asm volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
  return x0;
#endif
}

long LinuxSyscall(long number, long a, long b) {
#if defined(__x86_64__)
  register long rax asm("rax") = number;
  register long rdi asm("rdi") = a;
  register long rsi asm("rsi") = b;
  asm volatile("syscall" : "+r"(rax) : "r"(rdi), "r"(rsi) : "rcx", "r11", "memory");
  return rax;
#elif defined(__aarch64__)
  register long x8 asm("x8") = number;
  register long x0 asm("x0") = a;
  register long x1 asm("x1") = b;
  asm volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1) : "memory");
  return x0;
#endif
}

long LinuxSyscall(long number, long a) {
#if defined(__x86_64__)
  register long rax asm("rax") = number;
  register long rdi asm("rdi") = a;
  asm volatile("syscall" : "+r"(rax) : "r"(rdi) : "rcx", "r11", "memory");
  return rax;
#elif defined(__aarch64__)
  register long x8 asm("x8") = number;
  register long x0 asm("x0") = a;
  asm volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
  return x0;
#endif
}

template <typename... Args>
long LinuxSyscall(long number, Args... args) {
  static_assert(sizeof...(Args) >= 1 && sizeof...(Args) <= 6);
  return LinuxSyscall(number, (long)args...);
}

}  // namespace

}  // namespace automat
