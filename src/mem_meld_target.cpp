// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#pragma maf main
#pragma maf add compile argument "-fno-sanitize=all"
#pragma maf add link argument "-fno-sanitize=all"

#include <dlfcn.h>
#include <fcntl.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <malloc.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

namespace {

using std::string;
using std::string_view;

constexpr uint64_t kPage = 4096;
constexpr int kMadvGuardInstall = 102;
constexpr int kMadvGuardRemove = 103;

uint64_t Pattern(uint64_t seed, uint64_t index) {
  uint64_t x = seed * 6364136223846793005ull + index * 1442695040888963407ull + 1;
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdull;
  x ^= x >> 33;
  return x;
}

struct Out {
  string buffer;
  Out& operator<<(string_view s) {
    buffer.append(s);
    return *this;
  }
  Out& operator<<(const char* s) { return *this << string_view(s); }
  Out& Dec(uint64_t v) {
    char tmp[24];
    auto end = std::to_chars(tmp, tmp + sizeof(tmp), v).ptr;
    buffer.append(tmp, end);
    return *this;
  }
  Out& Hex(uint64_t v) {
    char tmp[18];
    tmp[0] = '0';
    tmp[1] = 'x';
    auto end = std::to_chars(tmp + 2, tmp + sizeof(tmp), v, 16).ptr;
    buffer.append(tmp, end);
    return *this;
  }
  void Flush() {
    buffer += '\n';
    (void)!write(STDOUT_FILENO, buffer.data(), buffer.size());
    buffer.clear();
  }
};

bool ParseU64(string_view token, uint64_t& out) {
  int base = 10;
  if (token.starts_with("0x") || token.starts_with("0X")) {
    token.remove_prefix(2);
    base = 16;
  }
  auto end = token.data() + token.size();
  auto result = std::from_chars(token.data(), end, out, base);
  return result.ec == std::errc() && result.ptr == end;
}

bool ParseI64(string_view token, int64_t& out) {
  auto end = token.data() + token.size();
  auto result = std::from_chars(token.data(), end, out, 10);
  return result.ec == std::errc() && result.ptr == end;
}

int ParseProt(string_view token) {
  if (token == "none") return PROT_NONE;
  if (token == "r") return PROT_READ;
  if (token == "rw") return PROT_READ | PROT_WRITE;
  if (token == "rx") return PROT_READ | PROT_EXEC;
  if (token == "rwx") return PROT_READ | PROT_WRITE | PROT_EXEC;
  return -1;
}

const char* ErrName() {
  const char* name = strerrorname_np(errno);
  return name ? name : "EUNKNOWN";
}

std::atomic<bool> churn_quit;
std::vector<pthread_t> churn_threads;

void* Churn(void*) {
  void* ring[32] = {};
  for (uint64_t i = 0; !churn_quit.load(std::memory_order_relaxed); ++i) {
    free(ring[i % 32]);
    size_t n = 64 + (Pattern(1, i) % 8000);
    void* p = malloc(n);
    memset(p, (int)i, n);
    ring[i % 32] = p;
  }
  for (void* p : ring) free(p);
  return nullptr;
}

double spin_seconds;

ssize_t SpinOnce(void*, const char*, size_t n) {
  timespec start, now;
  clock_gettime(CLOCK_MONOTONIC, &start);
  do clock_gettime(CLOCK_MONOTONIC, &now);
  while (now.tv_sec - start.tv_sec + (now.tv_nsec - start.tv_nsec) * 1e-9 < spin_seconds);
  spin_seconds = 0;
  return (ssize_t)n;
}

char* argv0;
size_t argv0_size;

pid_t forking_thread;

int hold_pipe = -1;
pid_t held_child;

pid_t expected_sender;
std::atomic<int> rt_signals, usr1_signals, foreign_signals;
std::atomic<long> rt_values;

void CountSignal(int sig, siginfo_t* info, void*) {
  if (info->si_pid != expected_sender) foreign_signals.fetch_add(1, std::memory_order_relaxed);
  if (sig == SIGUSR1) {
    usr1_signals.fetch_add(1, std::memory_order_relaxed);
  } else {
    rt_signals.fetch_add(1, std::memory_order_relaxed);
    rt_values.fetch_add(info->si_value.sival_int, std::memory_order_relaxed);
  }
}

void OnOtherThread(void (*fn)()) {
  pthread_t thread;
  pthread_create(
      &thread, nullptr,
      [](void* fn) -> void* {
        ((void (*)())fn)();
        return nullptr;
      },
      (void*)fn);
  pthread_join(thread, nullptr);
}

struct Race {
  char* addr;
  uint64_t len, rounds;
  pthread_barrier_t barrier;
  bool map;
};

void* RaceThread(void* arg) {
  auto& race = *(Race*)arg;
  bool map = __atomic_exchange_n(&race.map, true, __ATOMIC_RELAXED);
  for (uint64_t i = 0; i < race.rounds; ++i) {
    pthread_barrier_wait(&race.barrier);
    if (map)
      mmap(race.addr, race.len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1,
           0);
    else
      munmap(race.addr, race.len);
    pthread_barrier_wait(&race.barrier);
  }
  return nullptr;
}

struct Stamps {
  uint64_t rounds;
  std::atomic<uint64_t> next, bad;
};

void* StampThread(void* arg) {
  auto& stamps = *(Stamps*)arg;
  uint64_t id = stamps.next.fetch_add(1, std::memory_order_relaxed) + 1;
  for (uint64_t i = 0; i < stamps.rounds; ++i) {
    auto* p = (uint64_t*)mmap(nullptr, 4 * kPage, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) continue;
    bool bad = false;
    for (uint64_t w = 0; w < 4 * kPage / 8; ++w) {
      bad |= p[w] != 0;
      p[w] = id << 32 | i;
    }
    for (uint64_t w = 0; w < 4 * kPage / 8; ++w) bad |= p[w] != (id << 32 | i);
    stamps.bad.fetch_add(bad, std::memory_order_relaxed);
    munmap(p, 4 * kPage);
  }
  return nullptr;
}

struct Growth {
  uint64_t rounds, grown;
  char* start;
  std::atomic<bool> done;
};

void* GrowBreak(void* arg) {
  auto& growth = *(Growth*)arg;
  for (; growth.grown < growth.rounds && sbrk(kPage) != (void*)-1; ++growth.grown)
    *(uint64_t*)(growth.start + growth.grown * kPage) = Pattern(7, growth.grown);
  growth.done.store(true, std::memory_order_relaxed);
  return nullptr;
}

struct Unmapper {
  uint64_t* addr;
  long delay_ns;
  std::atomic<bool> go;
};

void* UnmapSoon(void* arg) {
  auto& unmapper = *(Unmapper*)arg;
  while (!unmapper.go.load(std::memory_order_relaxed)) {
  }
  timespec start, now;
  clock_gettime(CLOCK_MONOTONIC, &start);
  do clock_gettime(CLOCK_MONOTONIC, &now);
  while ((now.tv_sec - start.tv_sec) * 1'000'000'000 + now.tv_nsec - start.tv_nsec <
         unmapper.delay_ns);
  munmap(unmapper.addr, 4 * kPage);
  return nullptr;
}

void* KillChild(void*) {
  char path[64];
  snprintf(path, sizeof path, "/proc/self/task/%d/children", (int)forking_thread);
  for (;;) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    char pid[32] = {};
    ssize_t n = read(fd, pid, sizeof pid - 1);
    close(fd);
    if (n > 0) {
      kill(atoi(pid), SIGKILL);
      return nullptr;
    }
  }
}

long RawSyscall(long number, long a, long b, long c, long d, long e, long f, bool& registers_kept) {
  register long rax asm("rax") = number;
  register long rdi asm("rdi") = a;
  register long rsi asm("rsi") = b;
  register long rdx asm("rdx") = c;
  register long r10 asm("r10") = d;
  register long r8 asm("r8") = e;
  register long r9 asm("r9") = f;
  asm volatile("syscall"
               : "+r"(rax), "+r"(rdi), "+r"(rsi), "+r"(rdx), "+r"(r10), "+r"(r8), "+r"(r9)
               :
               : "rcx", "r11", "memory");
  registers_kept = rdi == a && rsi == b && rdx == c && r10 == d && r8 == e && r9 == f;
  return rax;
}

struct CloneArgs {
  uint64_t flags, pidfd, child_tid, parent_tid, exit_signal, stack, stack_size, tls;
};

int RawFork(bool clone3, long extra_flags) {
  auto* tid = (volatile int*)malloc(64);
  auto* data = (volatile char*)malloc(64);
  *tid = 0x11111111;
  data[0] = 'P';
  long flags = CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID | extra_flags;
  CloneArgs args = {.flags = (uint64_t)flags, .child_tid = (uint64_t)tid, .exit_signal = SIGCHLD};
  long child = clone3 ? syscall(SYS_clone3, &args, sizeof args)
                      : syscall(SYS_clone, flags | SIGCHLD, 0, 0, tid, 0);
  if (child == 0) {
    int code = (*tid == syscall(SYS_gettid) ? 0 : 1) | (data[0] == 'P' ? 0 : 2);
    data[0] = 'C';
    syscall(SYS_exit, code);
  }
  int status = 0;
  waitpid((pid_t)child, &status, 0);
  int code = WIFEXITED(status) ? WEXITSTATUS(status) : 255;
  if (*tid != 0x11111111 || data[0] != 'P') code |= 4;
  free((void*)tid);
  free((void*)data);
  return code;
}

int ExecTrue(void*) {
  execl("/bin/true", "true", (char*)nullptr);
  return 99;
}

uint64_t recursion_leaf;
[[gnu::noinline]] uint64_t Recurse(uint64_t depth, uint64_t budget_words) {
  volatile uint64_t frame[64];
  for (auto& w : frame) w = depth;
  if (depth >= budget_words / 64) {
    recursion_leaf = frame[depth % 64];
    return depth;
  }
  return Recurse(depth + 1, budget_words);
}

bool ReadLine(string& line) {
  line.clear();
  char c;
  for (;;) {
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n == 1) {
      if (c == '\n') return true;
      line += c;
    } else if (n == 0) {
      return !line.empty();
    } else if (errno != EINTR) {
      return !line.empty();
    }
  }
}

std::vector<string_view> Split(string_view line) {
  std::vector<string_view> tokens;
  size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && line[i] == ' ') ++i;
    size_t start = i;
    while (i < line.size() && line[i] != ' ') ++i;
    if (i > start) tokens.push_back(line.substr(start, i - start));
  }
  return tokens;
}

void Handle(const std::vector<string_view>& t, Out& out) {
  int entry_errno = errno;
  auto cmd = t[0];
  auto fail = [&](const char* call) { out << "err " << ErrName() << " " << call; };

  if (cmd == "ping") {
    out << "ok";
  } else if (cmd == "errno" && t.size() == 1) {
    out << "ok ";
    out.Dec(entry_errno);
  } else if (cmd == "errno" && t.size() == 2) {
    uint64_t value;
    if (!ParseU64(t[1], value)) return (void)(out << "err args");
    errno = (int)value;
    out << "ok";
  } else if (cmd == "mmap" && (t.size() == 3 || t.size() == 4)) {
    uint64_t len, addr = 0;
    int prot = ParseProt(t[2]);
    if (!ParseU64(t[1], len) || prot < 0 || (t.size() == 4 && !ParseU64(t[3], addr)))
      return (void)(out << "err args");
    void* p =
        mmap((void*)addr, len, prot, MAP_PRIVATE | MAP_ANONYMOUS | (addr ? MAP_FIXED : 0), -1, 0);
    if (p == MAP_FAILED) return fail("mmap");
    out << "ok ";
    out.Hex((uint64_t)p);
  } else if (cmd == "rawmmap" && t.size() == 3) {
    uint64_t len;
    int prot = ParseProt(t[2]);
    if (!ParseU64(t[1], len) || prot < 0) return (void)(out << "err args");
    long p = syscall(SYS_mmap, 0, len, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == -1) return fail("mmap");
    out << "ok ";
    out.Hex((uint64_t)p);
  } else if (cmd == "rawmmapregisters") {
    bool registers_kept;
    long p = RawSyscall(SYS_mmap, 0, kPage, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1,
                        0, registers_kept);
    if (p < 0) return (void)(out << "err mmap");
    syscall(SYS_munmap, p, kPage);
    out << (registers_kept ? "ok" : "err registers");
  } else if (cmd == "rawmadviseregisters" && t.size() == 3) {
    uint64_t addr, len;
    if (!ParseU64(t[1], addr) || !ParseU64(t[2], len)) return (void)(out << "err args");
    bool registers_kept;
    long r = RawSyscall(SYS_madvise, (long)addr, (long)len, MADV_DONTNEED, 0, 0, 0, registers_kept);
    if (r < 0) return (void)(out << "err madvise");
    out << (registers_kept ? "ok" : "err registers");
  } else if (cmd == "mmapshared" && t.size() == 2) {
    uint64_t len;
    if (!ParseU64(t[1], len)) return (void)(out << "err args");
    void* p = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return fail("mmap");
    out << "ok ";
    out.Hex((uint64_t)p);
  } else if (cmd == "mmapfile" && (t.size() == 4 || t.size() == 5)) {
    uint64_t len, addr = 0;
    int prot = ParseProt(t[3]);
    if (!ParseU64(t[2], len) || prot < 0 || (t.size() == 5 && !ParseU64(t[4], addr)))
      return (void)(out << "err args");
    string path(t[1]);
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return fail("open");
    void* p = mmap((void*)addr, len, prot, MAP_PRIVATE | (addr ? MAP_FIXED : 0), fd, 0);
    close(fd);
    if (p == MAP_FAILED) return fail("mmap");
    out << "ok ";
    out.Hex((uint64_t)p);
  } else if (cmd == "mmapfileshared" && t.size() == 3) {
    uint64_t len;
    if (!ParseU64(t[2], len)) return (void)(out << "err args");
    string path(t[1]);
    int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) return fail("open");
    void* p = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return fail("mmap");
    out << "ok ";
    out.Hex((uint64_t)p);
  } else if (cmd == "munmap" && t.size() == 3) {
    uint64_t addr, len;
    if (!ParseU64(t[1], addr) || !ParseU64(t[2], len)) return (void)(out << "err args");
    if (munmap((void*)addr, len)) return fail("munmap");
    out << "ok";
  } else if ((cmd == "mremap" || cmd == "mremapmove") && t.size() == 4) {
    uint64_t addr, old_len, new_len;
    if (!ParseU64(t[1], addr) || !ParseU64(t[2], old_len) || !ParseU64(t[3], new_len))
      return (void)(out << "err args");
    void* p;
    if (cmd == "mremapmove") {
      void* slot = mmap(nullptr, new_len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (slot == MAP_FAILED) return fail("mmap");
      p = mremap((void*)addr, old_len, new_len, MREMAP_MAYMOVE | MREMAP_FIXED, slot);
    } else {
      p = mremap((void*)addr, old_len, new_len, MREMAP_MAYMOVE);
    }
    if (p == MAP_FAILED) return fail("mremap");
    out << "ok ";
    out.Hex((uint64_t)p);
  } else if (cmd == "mremapfixed" && t.size() == 5) {
    uint64_t addr, old_len, new_len, to;
    if (!ParseU64(t[1], addr) || !ParseU64(t[2], old_len) || !ParseU64(t[3], new_len) ||
        !ParseU64(t[4], to))
      return (void)(out << "err args");
    void* p = mremap((void*)addr, old_len, new_len, MREMAP_MAYMOVE | MREMAP_FIXED, (void*)to);
    if (p == MAP_FAILED) return fail("mremap");
    out << "ok ";
    out.Hex((uint64_t)p);
  } else if (cmd == "mremapdontunmap" && t.size() == 3) {
    uint64_t addr, len;
    if (!ParseU64(t[1], addr) || !ParseU64(t[2], len)) return (void)(out << "err args");
    void* p = mremap((void*)addr, len, len, MREMAP_MAYMOVE | MREMAP_DONTUNMAP);
    if (p == MAP_FAILED) return fail("mremap");
    out << "ok ";
    out.Hex((uint64_t)p);
  } else if (cmd == "mprotect" && t.size() == 4) {
    uint64_t addr, len;
    int prot = ParseProt(t[3]);
    if (!ParseU64(t[1], addr) || !ParseU64(t[2], len) || prot < 0) return (void)(out << "err args");
    if (mprotect((void*)addr, len, prot)) return fail("mprotect");
    out << "ok";
  } else if ((cmd == "brk" || cmd == "sbrk") && t.size() == 2) {
    int64_t delta;
    if (!ParseI64(t[1], delta)) return (void)(out << "err args");
    void* prev = sbrk(delta);
    if (prev == (void*)-1) return fail("sbrk");
    out << "ok ";
    out.Hex(cmd == "sbrk" ? (uint64_t)prev : (uint64_t)sbrk(0));
  } else if (cmd == "malloc" && t.size() == 2) {
    uint64_t len;
    if (!ParseU64(t[1], len)) return (void)(out << "err args");
    void* p = malloc(len);
    if (!p) return fail("malloc");
    out << "ok ";
    out.Hex((uint64_t)p);
  } else if (cmd == "calloc" && t.size() == 3) {
    uint64_t n, size;
    if (!ParseU64(t[1], n) || !ParseU64(t[2], size)) return (void)(out << "err args");
    void* p = calloc(n, size);
    if (!p) return fail("calloc");
    out << "ok ";
    out.Hex((uint64_t)p);
  } else if (cmd == "free" && t.size() == 2) {
    uint64_t addr;
    if (!ParseU64(t[1], addr)) return (void)(out << "err args");
    free((void*)addr);
    out << "ok";
  } else if (cmd == "malloc_trim") {
    malloc_trim(0);
    out << "ok";
  } else if (cmd == "mallocstats" && t.size() == 2) {
    uint64_t ms;
    if (!ParseU64(t[1], ms)) return (void)(out << "err args");
    spin_seconds = ms * 1e-3;
    FILE* saved = stderr;
    stderr = fopencookie(nullptr, "w", {.write = SpinOnce});
    setvbuf(stderr, nullptr, _IONBF, 0);
    malloc_stats();
    fclose(stderr);
    stderr = saved;
    out << "ok";
  } else if (cmd == "madvise" && t.size() == 4) {
    uint64_t addr, len;
    int advice = t[3] == "dontneed"         ? MADV_DONTNEED
                 : t[3] == "free"           ? MADV_FREE
                 : t[3] == "remove"         ? MADV_REMOVE
                 : t[3] == "wipeonfork"     ? MADV_WIPEONFORK
                 : t[3] == "dontfork"       ? MADV_DONTFORK
                 : t[3] == "nohugepage"     ? MADV_NOHUGEPAGE
                 : t[3] == "dontneedlocked" ? MADV_DONTNEED_LOCKED
                 : t[3] == "guard"          ? kMadvGuardInstall
                 : t[3] == "unguard"        ? kMadvGuardRemove
                                            : -1;
    if (!ParseU64(t[1], addr) || !ParseU64(t[2], len) || advice < 0)
      return (void)(out << "err args");
    if (madvise((void*)addr, len, advice)) return fail("madvise");
    out << "ok";
  } else if (cmd == "write" && t.size() == 4) {
    uint64_t addr, len, seed;
    if (!ParseU64(t[1], addr) || !ParseU64(t[2], len) || !ParseU64(t[3], seed))
      return (void)(out << "err args");
    auto* p = (uint8_t*)addr;
    for (uint64_t i = 0; i < len; ++i) p[i] = (uint8_t)Pattern(seed, i);
    out << "ok";
  } else if (cmd == "verify" && t.size() == 4) {
    uint64_t addr, len, seed;
    if (!ParseU64(t[1], addr) || !ParseU64(t[2], len) || !ParseU64(t[3], seed))
      return (void)(out << "err args");
    auto* p = (uint8_t*)addr;
    for (uint64_t i = 0; i < len; ++i)
      if (p[i] != (uint8_t)Pattern(seed, i)) {
        out << "mismatch ";
        out.Dec(i);
        return;
      }
    out << "ok";
  } else if (cmd == "threads" && t.size() == 2) {
    uint64_t n;
    if (!ParseU64(t[1], n)) return (void)(out << "err args");
    churn_quit.store(false, std::memory_order_relaxed);
    for (uint64_t i = 0; i < n; ++i) {
      pthread_t th;
      if (pthread_create(&th, nullptr, Churn, nullptr) == 0) churn_threads.push_back(th);
    }
    out << "ok";
  } else if (cmd == "threads_stop") {
    churn_quit.store(true, std::memory_order_relaxed);
    for (pthread_t th : churn_threads) pthread_join(th, nullptr);
    churn_threads.clear();
    out << "ok";
  } else if (cmd == "recurse" && t.size() == 2) {
    uint64_t kb;
    if (!ParseU64(t[1], kb)) return (void)(out << "err args");
    uint64_t depth = Recurse(0, kb * 1024 / 8);
    out << "ok depth=";
    out.Dec(depth);
  } else if (cmd == "fork") {
    int pipefd[2];
    if (pipe(pipefd)) return fail("pipe");
    volatile uint64_t marker = 0x1234;
    auto* heap = (uint64_t*)malloc(8);
    *heap = 0xabcd;
    pid_t c = fork();
    if (c == 0) {
      marker = 0x5678;
      *heap = 0xbeef;
      void* blocks[64];
      for (auto& b : blocks) b = malloc(4096);
      for (auto& b : blocks) free(b);
      char ok = (marker == 0x5678 && *heap == 0xbeef) ? '1' : '0';
      (void)!write(pipefd[1], &ok, 1);
      _exit(0);
    }
    close(pipefd[1]);
    char child_ok = '0';
    (void)!read(pipefd[0], &child_ok, 1);
    close(pipefd[0]);
    int status = 0;
    waitpid(c, &status, 0);
    bool parent_ok = marker == 0x1234 && *heap == 0xabcd;
    free(heap);
    out << (parent_ok && child_ok == '1' ? "ok" : "err isolation");
  } else if (cmd == "forkexec") {
    pid_t c = fork();
    if (c == 0) {
      execl("/bin/true", "true", (char*)nullptr);
      _exit(99);
    }
    int status = 0;
    waitpid(c, &status, 0);
    out << "ok status=";
    out.Dec(WIFEXITED(status) ? WEXITSTATUS(status) : 255);
  } else if (cmd == "forktwice") {
    rlimit saved;
    getrlimit(RLIMIT_NOFILE, &saved);
    rlimit none = {3, saved.rlim_max};
    setrlimit(RLIMIT_NOFILE, &none);
    pid_t c = fork();
    if (c == 0) {
      pid_t g = fork();
      if (g == 0) _exit(0);
      int status = 1;
      waitpid(g, &status, 0);
      _exit(WIFEXITED(status) ? WEXITSTATUS(status) : 1);
    }
    setrlimit(RLIMIT_NOFILE, &saved);
    int status = 0;
    waitpid(c, &status, 0);
    out << "ok status=";
    out.Dec(WIFEXITED(status) ? WEXITSTATUS(status) : 255);
  } else if (cmd == "forkkilled" && t.size() == 2) {
    uint64_t len;
    if (!ParseU64(t[1], len)) return (void)(out << "err args");
    void* p = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return fail("mmap");
    memset(p, 1, len);
    signal(SIGCHLD, SIG_IGN);
    forking_thread = gettid();
    pthread_t killer;
    pthread_create(&killer, nullptr, KillChild, nullptr);
    if (fork() == 0) _exit(0);
    pthread_join(killer, nullptr);
    signal(SIGCHLD, SIG_DFL);
    munmap(p, len);
    out << "ok";
  } else if (cmd == "forktest") {
    int pipefd[2];
    if (pipe(pipefd)) return fail("pipe");
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_t mutex;
    pthread_mutex_init(&mutex, &attr);
    pthread_mutex_lock(&mutex);
    pid_t c = fork();
    if (c == 0) {
      unsigned char bytes[16];
      getrandom(bytes, sizeof(bytes), 0);
      (void)!write(pipefd[1], bytes, sizeof(bytes));
      _exit(0);
    }
    close(pipefd[1]);
    int relock = pthread_mutex_trylock(&mutex);
    unsigned char parent_bytes[16];
    getrandom(parent_bytes, sizeof(parent_bytes), 0);
    unsigned char child_bytes[16] = {};
    (void)!read(pipefd[0], child_bytes, sizeof(child_bytes));
    close(pipefd[0]);
    waitpid(c, nullptr, 0);
    out << "ok mutex=" << (relock == 0 ? "ok" : "busy") << " parent=";
    for (auto b : parent_bytes)
      out.buffer += "0123456789abcdef"[b >> 4], out.buffer += "0123456789abcdef"[b & 15];
    out << " child=";
    for (auto b : child_bytes)
      out.buffer += "0123456789abcdef"[b >> 4], out.buffer += "0123456789abcdef"[b & 15];
  } else if (cmd == "forkhold" && t.size() == 4) {
    uint64_t addr, len, seed;
    if (!ParseU64(t[1], addr) || !ParseU64(t[2], len) || !ParseU64(t[3], seed))
      return (void)(out << "err args");
    int ready[2], hold[2];
    if (pipe(ready) || pipe(hold)) return fail("pipe");
    pid_t c = fork();
    if (c == 0) {
      close(hold[1]);
      auto* p = (uint8_t*)addr;
      for (uint64_t i = 0; i < len; ++i) p[i] = (uint8_t)Pattern(seed, i);
      char b = 1;
      (void)!write(ready[1], &b, 1);
      (void)!read(hold[0], &b, 1);
      _exit(0);
    }
    close(ready[1]);
    close(hold[0]);
    char b;
    (void)!read(ready[0], &b, 1);
    close(ready[0]);
    hold_pipe = hold[1];
    held_child = c;
    out << "ok ";
    out.Dec(c);
  } else if (cmd == "forkrelease") {
    close(hold_pipe);
    int status = 0;
    waitpid(held_child, &status, 0);
    out << "ok status=";
    out.Dec(WIFEXITED(status) ? WEXITSTATUS(status) : 255);
  } else if (cmd == "rawfork" && t.size() == 2 &&
             (t[1] == "clone" || t[1] == "clone3" || t[1] == "clonefiles" || t[1] == "untraced")) {
    out << "ok status=";
    out.Dec(RawFork(t[1] == "clone3", t[1] == "clonefiles" ? CLONE_FILES
                                      : t[1] == "untraced" ? CLONE_UNTRACED
                                                           : 0));
  } else if (cmd == "forkunmap" && t.size() == 2) {
    uint64_t rounds, bad = 0;
    if (!ParseU64(t[1], rounds)) return (void)(out << "err args");
    for (uint64_t i = 0; i < rounds; ++i) {
      Unmapper unmapper = {.delay_ns = 10'000 * (long)(i % 10)};
      unmapper.addr = (uint64_t*)mmap(nullptr, 4 * kPage, PROT_READ | PROT_WRITE,
                                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (unmapper.addr == MAP_FAILED) return fail("mmap");
      for (uint64_t w = 0; w < 4 * kPage / 8; ++w) unmapper.addr[w] = Pattern(i, w);
      pthread_t thread;
      pthread_create(&thread, nullptr, UnmapSoon, &unmapper);
      unmapper.go.store(true, std::memory_order_relaxed);
      pid_t c = fork();
      if (c == 0) {
        unsigned char resident[4];
        if (mincore(unmapper.addr, 4 * kPage, resident) != 0) _exit(0);
        for (uint64_t w = 0; w < 4 * kPage / 8; ++w)
          if (unmapper.addr[w] != Pattern(i, w)) _exit(1);
        _exit(0);
      }
      pthread_join(thread, nullptr);
      int status = 0;
      waitpid(c, &status, 0);
      bad += !WIFEXITED(status) || WEXITSTATUS(status) != 0;
    }
    if (bad) {
      out << "mismatch ";
      out.Dec(bad);
    } else {
      out << "ok";
    }
  } else if (cmd == "forkshared" && t.size() == 2) {
    uint64_t addr;
    if (!ParseU64(t[1], addr)) return (void)(out << "err args");
    pid_t c = fork();
    if (c == 0) {
      memcpy((char*)addr, "CHILD", 6);
      _exit(0);
    }
    waitpid(c, nullptr, 0);
    out << (memcmp((char*)addr, "CHILD", 6) == 0 ? "ok" : "err unshared");
  } else if (cmd == "forkfd" && t.size() == 2) {
    uint64_t fd;
    if (!ParseU64(t[1], fd)) return (void)(out << "err args");
    pid_t c = fork();
    if (c == 0) _exit(fcntl((int)fd, F_GETFD) != -1);
    int status = 0;
    waitpid(c, &status, 0);
    out << "ok open=";
    out.Dec(WIFEXITED(status) ? WEXITSTATUS(status) : 255);
  } else if (cmd == "vfork") {
    pid_t c = vfork();
    if (c == 0) {
      execl("/bin/true", "true", (char*)nullptr);
      _exit(99);
    }
    int status = 0;
    waitpid(c, &status, 0);
    out << "ok status=";
    out.Dec(WIFEXITED(status) ? WEXITSTATUS(status) : 255);
  } else if (cmd == "clonevm") {
    static char stack[64 << 10];
    pid_t c = clone(ExecTrue, stack + sizeof stack, CLONE_VM | SIGCHLD, nullptr);
    if (c < 0) return fail("clone");
    int status = 0;
    waitpid(c, &status, 0);
    out << "ok status=";
    out.Dec(WIFEXITED(status) ? WEXITSTATUS(status) : 255);
  } else if (cmd == "getpids" && t.size() == 2) {
    uint64_t n;
    if (!ParseU64(t[1], n)) return (void)(out << "err args");
    for (uint64_t i = 0; i < n; ++i) syscall(SYS_getpid);
    out << "ok";
  } else if (cmd == "mmaps" && t.size() == 2) {
    uint64_t n;
    if (!ParseU64(t[1], n)) return (void)(out << "err args");
    for (uint64_t i = 0; i < n; ++i) {
      long p =
          syscall(SYS_mmap, 0, kPage, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (p == -1) return fail("mmap");
      syscall(SYS_munmap, p, kPage);
    }
    out << "ok";
  } else if (cmd == "remaps" && t.size() == 2) {
    uint64_t n;
    if (!ParseU64(t[1], n)) return (void)(out << "err args");
    for (uint64_t i = 0; i < n; ++i) {
      uint64_t len = (1 + i % 8) * kPage;
      void* p = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (p == MAP_FAILED) return fail("mmap");
      memset(p, (int)i, len);
      void* slot = mmap(nullptr, 2 * len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (slot == MAP_FAILED) return fail("mmap");
      void* moved = mremap(p, len, 2 * len, MREMAP_MAYMOVE | MREMAP_FIXED, slot);
      if (moved == MAP_FAILED) return fail("mremap");
      munmap(moved, 2 * len);
    }
    out << "ok";
  } else if (cmd == "mapstamps" && t.size() == 3) {
    uint64_t threads;
    Stamps stamps = {};
    if (!ParseU64(t[1], threads) || !ParseU64(t[2], stamps.rounds))
      return (void)(out << "err args");
    std::vector<pthread_t> workers(threads);
    for (pthread_t& worker : workers) pthread_create(&worker, nullptr, StampThread, &stamps);
    for (pthread_t worker : workers) pthread_join(worker, nullptr);
    if (uint64_t bad = stamps.bad.load(std::memory_order_relaxed)) {
      out << "mismatch ";
      out.Dec(bad);
    } else {
      out << "ok";
    }
  } else if (cmd == "brkrace" && t.size() == 2) {
    Growth growth = {.start = (char*)sbrk(0)};
    uint64_t lost = 0;
    if (!ParseU64(t[1], growth.rounds)) return (void)(out << "err args");
    pthread_t grower;
    pthread_create(&grower, nullptr, GrowBreak, &growth);
    while (!growth.done.load(std::memory_order_relaxed)) syscall(SYS_brk, 0);
    pthread_join(grower, nullptr);
    for (uint64_t i = 0; i < growth.grown; ++i)
      lost += *(uint64_t*)(growth.start + i * kPage) != Pattern(7, i);
    sbrk(-(intptr_t)(growth.grown * kPage));
    if (lost) {
      out << "mismatch ";
      out.Dec(lost);
    } else {
      out << "ok";
    }
  } else if (cmd == "racemaps" && t.size() == 2) {
    Race race = {.len = 65536};
    if (!ParseU64(t[1], race.rounds)) return (void)(out << "err args");
    race.addr =
        (char*)mmap(nullptr, race.len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (race.addr == MAP_FAILED) return fail("mmap");
    pthread_barrier_init(&race.barrier, nullptr, 3);
    pthread_t threads[2];
    for (pthread_t& thread : threads) pthread_create(&thread, nullptr, RaceThread, &race);
    uint64_t bad = 0;
    unsigned char resident[16];
    for (uint64_t i = 0; i < race.rounds; ++i) {
      pthread_barrier_wait(&race.barrier);
      pthread_barrier_wait(&race.barrier);
      if (mincore(race.addr, race.len, resident) != 0) continue;
      auto* p = (uint64_t*)race.addr;
      for (uint64_t w = 0; w < race.len / 8; ++w) p[w] = Pattern(i, w);
      for (uint64_t w = 0; w < race.len / 8 && !bad; ++w)
        if (p[w] != Pattern(i, w)) bad = i + 1;
    }
    for (pthread_t thread : threads) pthread_join(thread, nullptr);
    pthread_barrier_destroy(&race.barrier);
    if (bad) {
      out << "mismatch ";
      out.Dec(bad - 1);
    } else {
      out << "ok ";
      out.Hex((uint64_t)race.addr);
    }
  } else if (cmd == "sigwatch" && t.size() == 3) {
    uint64_t pid, rt_signal;
    if (!ParseU64(t[1], pid) || !ParseU64(t[2], rt_signal)) return (void)(out << "err args");
    expected_sender = (pid_t)pid;
    rt_signals = usr1_signals = foreign_signals = 0;
    rt_values = 0;
    struct sigaction action = {};
    action.sa_sigaction = CountSignal;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGUSR1, &action, nullptr);
    sigaction((int)rt_signal, &action, nullptr);
    out << "ok";
  } else if (cmd == "sigcount") {
    out << "ok rt=";
    out.Dec(rt_signals.load(std::memory_order_relaxed));
    out << " values=";
    out.Dec(rt_values.load(std::memory_order_relaxed));
    out << " usr1=";
    out.Dec(usr1_signals.load(std::memory_order_relaxed));
    out << " foreign=";
    out.Dec(foreign_signals.load(std::memory_order_relaxed));
  } else if (cmd == "close" && t.size() == 2) {
    uint64_t fd;
    if (!ParseU64(t[1], fd)) return (void)(out << "err args");
    if (close((int)fd)) return fail("close");
    out << "ok";
  } else if (cmd == "seccomp") {
    sock_filter allow = BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    sock_fprog program = {.len = 1, .filter = &allow};
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) ||
        syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &program))
      return fail("seccomp");
    out << "ok";
  } else if (cmd == "setarg" && t.size() == 2) {
    size_t n = std::min(t[1].size(), argv0_size);
    memcpy(argv0, t[1].data(), n);
    memset(argv0 + n, 0, argv0_size - n);
    out << "ok";
  } else if (cmd == "dlopen" && t.size() == 2) {
    string path(t[1]);
    void* handle = dlopen(path.c_str(), RTLD_NOW);
    if (!handle) return (void)(out << "err dlopen");
    out << "ok handle=";
    out.Hex((uint64_t)handle);
  } else if (cmd == "getrandom" && t.size() == 2) {
    uint64_t len;
    if (!ParseU64(t[1], len) || len > 256) return (void)(out << "err args");
    unsigned char bytes[256];
    if (getrandom(bytes, len, 0) != (ssize_t)len) return fail("getrandom");
    out << "ok ";
    for (uint64_t i = 0; i < len; ++i)
      out.buffer += "0123456789abcdef"[bytes[i] >> 4],
          out.buffer += "0123456789abcdef"[bytes[i] & 15];
  } else if (cmd == "exec" && t.size() == 2) {
    string path(t[1]);
    execl(path.c_str(), path.c_str(), (char*)nullptr);
    out << "err exec";
  } else if (cmd == "threadexec" && t.size() == 2) {
    static string path;
    path = t[1];
    OnOtherThread([] { execl(path.c_str(), path.c_str(), (char*)nullptr); });
    out << "err exec";
  } else if (cmd == "exit" && t.size() == 2) {
    uint64_t code;
    ParseU64(t[1], code);
    _exit((int)code);
  } else if (cmd == "threadexit" && t.size() == 2) {
    static uint64_t code;
    ParseU64(t[1], code);
    OnOtherThread([] { _exit((int)code); });
  } else {
    out << "err unknown";
  }
}

}  // namespace

int main(int, char** argv) {
  argv0 = argv[0];
  argv0_size = strlen(argv[0]);
  Out out;
  out << "pid ";
  out.Dec(getpid());
  out.Flush();

  string line;
  while (ReadLine(line)) {
    auto tokens = Split(line);
    if (tokens.empty()) continue;
    Handle(tokens, out);
    out.Flush();
  }
  churn_quit.store(true, std::memory_order_relaxed);
  for (pthread_t th : churn_threads) pthread_join(th, nullptr);
  return 0;
}
