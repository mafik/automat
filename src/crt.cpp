// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include "crt.hpp"

#if defined(__linux__)
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <functional>
#include <span>
#include <system_error>

#include "format.hpp"
#include "linux_syscall.hpp"

extern "C" {
void __pthread_attach_host(void* stack_addr, size_t stack_size, size_t guard_size);
void __pthread_detach_host(void);
}

namespace automat::crt {

namespace {

struct HelperFunctions {
  void* (*open)(const char*, int);
  void* (*find)(void*, const char*);
  int (*close)(void*);
  char* (*error)();
} host;

int* (*host_errno_location)();

}  // namespace

void* OpenLibrary(const char* name, int mode) {
  void* library = host.open(name, mode);
  if (!library) errno = *host_errno_location();
  return library;
}
void* FindSymbol(void* library, const char* name) { return host.find(library, name); }
int CloseLibrary(void* library) { return host.close(library); }
const char* LibraryError() { return host.error(); }

}  // namespace automat::crt

extern "C" {
void* dlopen(const char* name, int mode) { return automat::crt::OpenLibrary(name, mode); }
void* dlsym(void* library, const char* name) {
  if (library == RTLD_DEFAULT) return nullptr;
  return automat::crt::FindSymbol(library, name);
}
__attribute__((weak)) int dlclose(void* library) { return automat::crt::CloseLibrary(library); }
char* dlerror() { return const_cast<char*>(automat::crt::LibraryError()); }
}

namespace automat::crt {

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
};

const unsigned char kHelperImage[] = {
#embed "../build/generated/crt_helper"
};

struct Source {
  const unsigned char* bytes;
  int fd;

  bool Read(void* destination, size_t size, size_t offset) const {
    if (bytes) {
      memcpy(destination, bytes + offset, size);
      return true;
    }
    return LinuxSyscall(__NR_pread64, fd, destination, size, offset) == (ssize_t)size;
  }
};

struct Image {
  char* base;
  Elf64_Ehdr header;
};

const char* start_error;

void MapImage(const Source& source, Image& image, char* interpreter, size_t interpreter_size) {
  const size_t page_size = sysconf(_SC_PAGESIZE);
  Elf64_Phdr headers[kMaxProgramHeaders];
  if (!source.Read(&image.header, sizeof(image.header), 0) ||
      memcmp(image.header.e_ident, ELFMAG, SELFMAG) || image.header.e_type != ET_DYN ||
      image.header.e_machine != EM_X86_64 || image.header.e_phentsize != sizeof(Elf64_Phdr) ||
      image.header.e_phnum > kMaxProgramHeaders ||
      !source.Read(headers, image.header.e_phnum * sizeof(Elf64_Phdr), image.header.e_phoff)) {
    start_error = "Not a dynamic x86-64 ELF image";
    return;
  }
  size_t size = 0;
  for (const Elf64_Phdr& header : std::span(headers, image.header.e_phnum)) {
    if (header.p_type == PT_LOAD) {
      size = std::max<size_t>(size, header.p_vaddr + header.p_memsz);
    }
  }
  size = (size + page_size - 1) & -page_size;
  long base =
      LinuxSyscall(__NR_mmap, 0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (base < 0) {
    start_error = "mmap";
    return;
  }
  image.base = (char*)base;
  for (const Elf64_Phdr& header : std::span(headers, image.header.e_phnum)) {
    if (header.p_type == PT_INTERP && interpreter_size) {
      if (header.p_filesz >= interpreter_size ||
          !source.Read(interpreter, header.p_filesz, header.p_offset)) {
        start_error = "Couldn't read the interpreter path";
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
        LinuxSyscall(__NR_mprotect, image.base + page_start, page_end - page_start, protection)) {
      start_error = "Couldn't map a segment";
      return;
    }
  }
}

struct HostThreadFunctions {
  int (*create)(unsigned long*, const void*, void* (*)(void*), void*);
  int (*join)(unsigned long, void**);
  int (*detach)(unsigned long);
  unsigned long (*self)();
  int (*getattr_np)(unsigned long, void*);
  int (*attr_getstack)(const void*, void**, size_t*);
  int (*attr_getguardsize)(const void*, size_t*);
  int (*attr_destroy)(void*);
} host_thread;

template <typename Function>
void FindHostFunction(const char* name, Function& function, Status& status) {
  function = reinterpret_cast<Function>(FindSymbol(nullptr, name));
  if (!function) {
    status() += f("{}: {}", name, LibraryError());
  }
}

void (*host_free)(void*);

void LoadHostFunctions(Status& status) {
  FindHostFunction("pthread_create", host_thread.create, status);
  FindHostFunction("pthread_join", host_thread.join, status);
  FindHostFunction("pthread_detach", host_thread.detach, status);
  FindHostFunction("pthread_self", host_thread.self, status);
  FindHostFunction("pthread_getattr_np", host_thread.getattr_np, status);
  FindHostFunction("pthread_attr_getstack", host_thread.attr_getstack, status);
  FindHostFunction("pthread_attr_getguardsize", host_thread.attr_getguardsize, status);
  FindHostFunction("pthread_attr_destroy", host_thread.attr_destroy, status);
  FindHostFunction("free", host_free, status);
  FindHostFunction("__errno_location", host_errno_location, status);
}

struct ChimeraLaunch {
  std::move_only_function<void()> body;
  sigset_t creator_signal_mask;
};

}  // namespace

extern "C" int __start_host(const Elf64_Phdr* tls) {
  Image program, interpreter;
  char interpreter_path[256] = {};
  MapImage({kHelperImage, -1}, program, interpreter_path, sizeof(interpreter_path));
  if (start_error) {
    return 0;
  }
  int fd = LinuxSyscall(__NR_open, interpreter_path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    start_error = "Couldn't open the interpreter";
    return 0;
  }
  MapImage({nullptr, fd}, interpreter, nullptr, 0);
  LinuxSyscall(__NR_close, fd);
  if (start_error) {
    return 0;
  }
  static Elf64_Phdr program_headers[kMaxProgramHeaders + 1];
  int program_header_count = program.header.e_phnum;
  memcpy(program_headers, program.base + program.header.e_phoff,
         program_header_count * sizeof(Elf64_Phdr));
  for (Elf64_Phdr& header : std::span(program_headers, program_header_count)) {
    if (header.p_type == PT_PHDR) {
      header.p_vaddr = header.p_paddr = (char*)program_headers - program.base;
      header.p_filesz = header.p_memsz = (program_header_count + 1) * sizeof(Elf64_Phdr);
    }
  }
  program_headers[program_header_count++] = {
      .p_type = PT_TLS,
      .p_flags = PF_R,
      .p_offset = 0,
      .p_vaddr = tls->p_vaddr - (uintptr_t)program.base,
      .p_paddr = tls->p_vaddr - (uintptr_t)program.base,
      .p_filesz = tls->p_filesz,
      .p_memsz = tls->p_memsz,
      .p_align = tls->p_align,
  };
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
  long stack = LinuxSyscall(__NR_mmap, 0, kHelperStackSize, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (stack < 0) {
    start_error = "mmap";
    return 0;
  }
  static jmp_buf return_to_start;
  auto receive_helper_functions = [](void** functions) {
    memcpy(&host, functions, sizeof(host));
    _longjmp(return_to_start, 1);
  };
  char* address_argument = (char*)stack + kHelperStackSize - 1;
  for (auto address = (unsigned long)(void (*)(void**))receive_helper_functions; address;
       address /= 10) {
    *--address_argument = '0' + address % 10;
  }
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
        value = (unsigned long)program_headers;
        break;
      case kAuxPhent:
        value = program.header.e_phentsize;
        break;
      case kAuxPhnum:
        value = program_header_count;
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
  if (!_setjmp(return_to_start)) {
    asm volatile("mov %1, %%rsp\n\tjmp *%0" : : "r"(entry), "r"(sp) : "memory");
  }
  return 1;
}

void Init(Status& status) {
  if (start_error) {
    status() += start_error;
    return;
  }
  LoadHostFunctions(status);
}

ChimeraThread::ChimeraThread(std::move_only_function<void()> body) {
  auto launch = std::make_unique<ChimeraLaunch>();
  launch->body = std::move(body);
  sigset_t all, creator_signal_mask;
  sigfillset(&all);
  sigprocmask(SIG_SETMASK, &all, &creator_signal_mask);
  launch->creator_signal_mask = creator_signal_mask;
  int error = host_thread.create(
      &handle, nullptr,
      [](void* argument) -> void* {
        auto* launch = (ChimeraLaunch*)argument;
        alignas(16) char attributes[64];
        void* stack = nullptr;
        size_t stack_size = 0;
        size_t guard_size = 0;
        if (!host_thread.getattr_np(host_thread.self(), attributes)) {
          host_thread.attr_getstack(attributes, &stack, &stack_size);
          host_thread.attr_getguardsize(attributes, &guard_size);
          host_thread.attr_destroy(attributes);
        }
        __pthread_attach_host(stack, stack_size, guard_size);
        sigprocmask(SIG_SETMASK, &launch->creator_signal_mask, nullptr);
        launch->body();
        delete launch;
        sigset_t all;
        sigfillset(&all);
        sigprocmask(SIG_BLOCK, &all, nullptr);
        __pthread_detach_host();
        return nullptr;
      },
      launch.get());
  sigprocmask(SIG_SETMASK, &creator_signal_mask, nullptr);
  if (error) {
    handle = 0;
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

void Free(void* pointer) { host_free(pointer); }

}  // namespace automat::crt

#endif
