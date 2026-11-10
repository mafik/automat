// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include "libc.hpp"

#if defined(__linux__)
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>

#include "format.hpp"
#include "linux_syscall.hpp"

#if defined(__COSMOPOLITAN__)
#include <functional>
#include <new>
#include <system_error>

#define _Bool bool
#include <libc/intrin/cxaatexit.h>
#include <libc/str/locale.internal.h>
#include <libc/thread/posixthread.internal.h>
#include <libc/thread/tls.h>
#undef _Bool
#endif

namespace automat::libc {

namespace {

struct HelperFunctions {
  void* (*open)(const char*, int);
  void* (*find)(void*, const char*);
  int (*close)(void*);
  char* (*error)();
} host;

}  // namespace

void* OpenLibrary(const char* name, int mode) { return host.open(name, mode); }
void* FindSymbol(void* library, const char* name) { return host.find(library, name); }
int CloseLibrary(void* library) { return host.close(library); }
const char* LibraryError() { return host.error(); }

}  // namespace automat::libc

namespace automat::libc {

namespace {

enum {
  kAuxPhdr = 3,
  kAuxPhent = 4,
  kAuxPhnum = 5,
  kAuxBase = 7,
  kAuxFlags = 8,
  kAuxEntry = 9,
  kAuxExecfn = 31,
  kHelperStackSize = 64 << 10,
  kMaxProgramHeaders = 16,
  kHostThreadStackSize = 8 << 20,
};

const unsigned char kHelperImage[] = {
#embed "../build/generated/libc_helper"
};

struct Source {
  const unsigned char* bytes;
  int fd;

  bool Read(void* destination, size_t size, size_t offset) const {
    if (bytes) {
      memcpy(destination, bytes + offset, size);
      return true;
    }
    return pread(fd, destination, size, offset) == (ssize_t)size;
  }
};

struct Image {
  char* base;
  Elf64_Ehdr header;
};

void MapImage(const Source& source, Image& image, char* interpreter, size_t interpreter_size,
              Status& status) {
  const size_t page_size = sysconf(_SC_PAGESIZE);
  Elf64_Phdr headers[kMaxProgramHeaders];
  if (!source.Read(&image.header, sizeof(image.header), 0) ||
      memcmp(image.header.e_ident, ELFMAG, SELFMAG) || image.header.e_type != ET_DYN ||
      image.header.e_machine != EM_X86_64 || image.header.e_phentsize != sizeof(Elf64_Phdr) ||
      image.header.e_phnum > kMaxProgramHeaders ||
      !source.Read(headers, image.header.e_phnum * sizeof(Elf64_Phdr), image.header.e_phoff)) {
    status() += "Not a dynamic x86-64 ELF image";
    return;
  }
  size_t size = 0;
  for (const Elf64_Phdr& header : std::span(headers, image.header.e_phnum)) {
    if (header.p_type == PT_LOAD) {
      size = std::max<size_t>(size, header.p_vaddr + header.p_memsz);
    }
  }
  size = (size + page_size - 1) & -page_size;
  void* base = mmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (base == MAP_FAILED) {
    status() += "mmap";
    return;
  }
  image.base = (char*)base;
  for (const Elf64_Phdr& header : std::span(headers, image.header.e_phnum)) {
    if (header.p_type == PT_INTERP && interpreter_size) {
      if (header.p_filesz >= interpreter_size ||
          !source.Read(interpreter, header.p_filesz, header.p_offset)) {
        status() += "Couldn't read the interpreter path";
        return;
      }
    }
    if (header.p_type != PT_LOAD) {
      continue;
    }
    size_t page_start = header.p_vaddr & -page_size;
    size_t page_end = (header.p_vaddr + header.p_memsz + page_size - 1) & -page_size;
    int protection = (header.p_flags & PF_R ? PROT_READ : 0) |
                     (header.p_flags & PF_W ? PROT_WRITE : 0) |
                     (header.p_flags & PF_X ? PROT_EXEC : 0);
    if (!source.Read(image.base + header.p_vaddr, header.p_filesz, header.p_offset) ||
        mprotect(image.base + page_start, page_end - page_start, protection)) {
      status() += "Couldn't map a segment";
      return;
    }
  }
}

struct HostThreadFunctions {
  int (*create)(unsigned long*, const void*, void* (*)(void*), void*);
  int (*join)(unsigned long, void**);
  int (*detach)(unsigned long);
} host_thread;

template <typename Function>
void FindHostFunction(const char* name, Function& function, Status& status) {
  function = reinterpret_cast<Function>(FindSymbol(nullptr, name));
  if (!function) {
    status() += f("{}: {}", name, LibraryError());
  }
}

void LoadHostThreadFunctions(Status& status) {
  FindHostFunction("pthread_create", host_thread.create, status);
  FindHostFunction("pthread_join", host_thread.join, status);
  FindHostFunction("pthread_detach", host_thread.detach, status);
}

#if defined(__x86_64__)
enum {
  kSysArchPrctl = 158,
  kSysGettid = 186,
  kArchSetGs = 0x1001,
  kArchGetGs = 0x1004,
};
#endif

#if defined(__COSMOPOLITAN__)
struct ChimeraLaunch {
  std::move_only_function<void()> body;
  PosixThread* posix_thread;
  sigset_t creator_signal_mask;
};
#endif

}  // namespace

void Init(Status& status) {
  if (getauxval(kAuxBase)) {
    host = {dlopen, dlsym, dlclose, dlerror};
    LoadHostThreadFunctions(status);
    return;
  }
  Image program, interpreter;
  char interpreter_path[256] = {};
  MapImage({kHelperImage, -1}, program, interpreter_path, sizeof(interpreter_path), status);
  if (!OK(status)) {
    return;
  }
  int fd = open(interpreter_path, O_RDONLY | O_CLOEXEC);
  if (fd == -1) {
    status() += f("open({})", interpreter_path);
    return;
  }
  MapImage({nullptr, fd}, interpreter, nullptr, 0, status);
  close(fd);
  if (!OK(status)) {
    return;
  }
  char** environment_end = environ;
  while (*environment_end) {
    ++environment_end;
  }
  auto* auxiliary_vector = (Elf64_auxv_t*)(environment_end + 1);
  int environment_count = environment_end - environ;
  int auxiliary_count = 0;
  while (auxiliary_vector[auxiliary_count].a_type) {
    ++auxiliary_count;
  }
  char* stack =
      (char*)mmap(0, kHelperStackSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (stack == MAP_FAILED) {
    status() += "mmap";
    return;
  }
  static jmp_buf return_to_init;
  auto receive_helper_functions = [](void** functions) {
    memcpy(&host, functions, sizeof(host));
    _longjmp(return_to_init, 1);
  };
  char* address_argument = stack + kHelperStackSize - 32;
  snprintf(address_argument, 32, "%lu", (unsigned long)(void (*)(void**))receive_helper_functions);
  size_t words = 4 + environment_count + 1 + 2 * auxiliary_count + 2;
  long* sp = (long*)(((uintptr_t)address_argument - words * sizeof(long)) & -16);
  long* out = sp;
  *out++ = 2;
  *out++ = (long)program_invocation_name;
  *out++ = (long)address_argument;
  *out++ = 0;
  for (int i = 0; i <= environment_count; ++i) {
    *out++ = (long)environ[i];
  }
  for (int i = 0; i < auxiliary_count; ++i) {
    unsigned long value = auxiliary_vector[i].a_un.a_val;
    switch (auxiliary_vector[i].a_type) {
      case kAuxPhdr:
        value = (unsigned long)program.base + program.header.e_phoff;
        break;
      case kAuxPhent:
        value = program.header.e_phentsize;
        break;
      case kAuxPhnum:
        value = program.header.e_phnum;
        break;
      case kAuxBase:
        value = (unsigned long)interpreter.base;
        break;
      case kAuxEntry:
        value = (unsigned long)program.base + program.header.e_entry;
        break;
      case kAuxFlags:
        value = 0;
        break;
      case kAuxExecfn:
        value = (unsigned long)program_invocation_name;
        break;
    }
    *out++ = auxiliary_vector[i].a_type;
    *out++ = value;
  }
  *out++ = 0;
  *out++ = 0;
  void* entry = interpreter.base + interpreter.header.e_entry;
#if defined(__x86_64__)
  unsigned long gs_base;
  LinuxSyscall(kSysArchPrctl, kArchGetGs, (long)&gs_base);
#endif
  if (_setjmp(return_to_init)) {
#if defined(__x86_64__)
    LinuxSyscall(kSysArchPrctl, kArchSetGs, gs_base);
#endif
    LoadHostThreadFunctions(status);
    return;
  }
  asm volatile("mov %1, %%rsp\n\tjmp *%0" : : "r"(entry), "r"(sp) : "memory");
}

#if defined(__COSMOPOLITAN__)
ChimeraThread::ChimeraThread(std::move_only_function<void()> body) {
  auto* posix_thread = (PosixThread*)calloc(1, sizeof(PosixThread));
  if (!posix_thread) {
    throw std::bad_alloc();
  }
  dll_init(&posix_thread->list);
  posix_thread->pt_locale = &__global_locale;
  posix_thread->pt_tls = _mktls(&posix_thread->tib);
  if (!posix_thread->pt_tls) {
    free(posix_thread);
    throw std::bad_alloc();
  }
  posix_thread->tib->tib_pthread = (pthread_t)posix_thread;
  __c11_atomic_store(&posix_thread->tib->tib_sigmask, -1, __ATOMIC_RELAXED);
  __c11_atomic_store(&posix_thread->pt_status, kPosixThreadDetached, __ATOMIC_RELAXED);
  auto launch = std::make_unique<ChimeraLaunch>();
  launch->body = std::move(body);
  launch->posix_thread = posix_thread;
  sigset_t all, creator_signal_mask;
  sigfillset(&all);
  sigprocmask(SIG_SETMASK, &all, &creator_signal_mask);
  launch->creator_signal_mask = creator_signal_mask;
  int error = host_thread.create(
      &handle, nullptr,
      [](void* argument) -> void* {
        auto* launch = (ChimeraLaunch*)argument;
        PosixThread* posix_thread = launch->posix_thread;
        LinuxSyscall(kSysArchPrctl, kArchSetGs, (long)posix_thread->tib);
        int tid = LinuxSyscall(kSysGettid, 0);
        __c11_atomic_store(&posix_thread->tib->tib_ptid, tid, __ATOMIC_RELAXED);
        __c11_atomic_store(&posix_thread->tib->tib_ctid, tid, __ATOMIC_RELAXED);
        _pthread_lock();
        dll_make_first(&_pthread_list, &posix_thread->list);
        _pthread_unlock();
        sigprocmask(SIG_SETMASK, &launch->creator_signal_mask, nullptr);
        launch->body();
        delete launch;
        __cxa_thread_finalize();
        sigset_t all;
        sigfillset(&all);
        sigprocmask(SIG_BLOCK, &all, nullptr);
        __c11_atomic_store(&posix_thread->pt_status, kPosixThreadZombie, __ATOMIC_RELEASE);
        _pthread_zombify(posix_thread);
        __c11_atomic_store(&posix_thread->tib->tib_ctid, 0, __ATOMIC_RELEASE);
        return nullptr;
      },
      launch.get());
  sigprocmask(SIG_SETMASK, &creator_signal_mask, nullptr);
  if (error) {
    handle = 0;
    free(posix_thread->pt_tls);
    free(posix_thread);
    throw std::system_error(error, std::generic_category());
  }
  launch.release();
}

ChimeraThread::ChimeraThread(ChimeraThread&& other) noexcept
    : handle(std::exchange(other.handle, 0)) {}

ChimeraThread& ChimeraThread::operator=(ChimeraThread&& other) noexcept {
  if (joinable()) {
    join();
  }
  handle = std::exchange(other.handle, 0);
  return *this;
}

ChimeraThread::~ChimeraThread() {
  if (joinable()) {
    join();
  }
}

void ChimeraThread::join() {
  host_thread.join(handle, nullptr);
  handle = 0;
}

void ChimeraThread::detach() {
  host_thread.detach(handle);
  handle = 0;
}
#endif

}  // namespace automat::libc

#endif
