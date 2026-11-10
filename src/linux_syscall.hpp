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
#elif defined(__aarch64__)
#define __NR_epoll_create1 20
#define __NR_epoll_ctl 21
#define __NR_epoll_pwait 22
#define __NR_eventfd2 19
#define __NR_timerfd_create 85
#define __NR_timerfd_settime 86
#define __NR_pidfd_open 434
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
