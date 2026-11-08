// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <malloc.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <thread>

#include "fd.hpp"
#include "format.hpp"
#include "gtest.hpp"
#include "log.hpp"
#include "mem_meld.hpp"
#include "path.hpp"
#include "thread_name.hpp"
#include "tracy_client.hpp"
#include "vec.hpp"

using namespace automat;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

constexpr uint64_t kPage = 4096;
constexpr uint64_t kMiB = 1ull << 20;
constexpr uint64_t kGiB = 1ull << 30;
constexpr uint64_t kPagemapGuardRegion = 1ull << 58;

uint64_t EnvU64(const char* name, uint64_t fallback) {
  const char* value = getenv(name);
  return value && *value ? strtoull(value, nullptr, 0) : fallback;
}

const uint64_t kSeconds = EnvU64("PM_STRESS_SECONDS", 2);
const uint64_t kRounds = EnvU64("PM_STRESS_ROUNDS", 12);
const uint64_t kThreads = EnvU64("PM_STRESS_THREADS", 24);
const uint64_t kMappings = EnvU64("PM_STRESS_MAPPINGS", 3000);
const uint64_t kLargeMiB = EnvU64("PM_STRESS_LARGE_MIB", 256);
const uint64_t kSeed = EnvU64("PM_STRESS_SEED", Clock::now().time_since_epoch().count());

double Seconds(Clock::duration d) { return std::chrono::duration<double>(d).count(); }
double Ms(Clock::duration d) { return Seconds(d) * 1000; }
double MiB(uint64_t bytes) { return bytes / (double)kMiB; }

void Report(const Str& text) { LOG << "STRESS " << text; }

uint8_t Pattern(uint64_t seed, uint64_t index) {
  uint64_t x = seed * 6364136223846793005ull + index * 1442695040888963407ull + 1;
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdull;
  x ^= x >> 33;
  return (uint8_t)x;
}

Str ReadFile(const Str& path) {
  int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return {};
  Str out;
  char buf[65536];
  for (ssize_t n; (n = read(fd, buf, sizeof buf)) > 0;) out.append(buf, n);
  close(fd);
  return out;
}

uint64_t KbField(const Str& text, StrView key) {
  size_t at = text.find(Str(key) + ":");
  if (at == Str::npos) return 0;
  return strtoull(text.c_str() + at + key.size() + 1, nullptr, 10) * 1024;
}

Vec<int> DirNumbers(const Str& path) {
  Vec<int> out;
  if (DIR* dir = opendir(path.c_str())) {
    while (dirent* e = readdir(dir))
      if (e->d_name[0] >= '0' && e->d_name[0] <= '9') out.push_back(atoi(e->d_name));
    closedir(dir);
  }
  return out;
}

uint64_t CpuNs(pid_t pid, pid_t tid) {
  return strtoull(ReadFile(f("/proc/{}/task/{}/schedstat", pid, tid)).c_str(), nullptr, 10);
}

pid_t TracerTid() {
  pid_t self = getpid();
  for (int tid : DirNumbers(f("/proc/{}/task", self)))
    if (ReadFile(f("/proc/{}/task/{}/comm", self, tid)).starts_with("MemMeld")) return tid;
  return 0;
}

struct Memory {
  uint64_t rss_anon = 0, rss_file = 0, rss_shmem = 0;

  static Memory Of(pid_t pid) {
    Str status = ReadFile(f("/proc/{}/status", pid));
    return {KbField(status, "RssAnon"), KbField(status, "RssFile"), KbField(status, "RssShmem")};
  }
  Str ToStr() const {
    return f("anon {:.1f} MiB, file {:.1f} MiB, shmem {:.1f} MiB", MiB(rss_anon), MiB(rss_file),
             MiB(rss_shmem));
  }
};

uint64_t MirrorMemfdBytes() {
  std::map<ino_t, uint64_t> memfds;
  for (int fd : DirNumbers("/proc/self/fd")) {
    char link[256];
    ssize_t n = readlink(f("/proc/self/fd/{}", fd).c_str(), link, sizeof link - 1);
    if (n <= 0) continue;
    link[n] = 0;
    struct stat st;
    if (strstr(link, "memfd:pm-mirror") && fstat(fd, &st) == 0)
      memfds[st.st_ino] = st.st_blocks * 512;
  }
  uint64_t sum = 0;
  for (auto& [inode, bytes] : memfds) sum += bytes;
  return sum;
}

struct Vma {
  uint64_t start = 0, end = 0;
  int prot = 0;
  bool shared = false;
  uint64_t inode = 0;
  Str path;
  Str flags;

  bool HasFlag(StrView token) const {
    for (size_t at = flags.find(token); at != Str::npos; at = flags.find(token, at + 1))
      if ((at == 0 || flags[at - 1] == ' ') &&
          (at + token.size() == flags.size() || flags[at + token.size()] == ' '))
        return true;
    return false;
  }
};

Vec<Vma> ReadMaps(const Str& path) {
  Vec<Vma> out;
  Str text = ReadFile(path);
  for (size_t pos = 0; pos < text.size();) {
    size_t nl = text.find('\n', pos);
    if (nl == Str::npos) nl = text.size();
    Str line = text.substr(pos, nl - pos);
    pos = nl + 1;
    uint64_t start, end, offset, inode;
    char perms[8] = {}, dev[16] = {};
    int name_at = 0;
    if (sscanf(line.c_str(), "%lx-%lx %7s %lx %15s %lu %n", &start, &end, perms, &offset, dev,
               &inode, &name_at) >= 6 &&
        (perms[0] == 'r' || perms[0] == '-')) {
      Vma v;
      v.start = start;
      v.end = end;
      v.prot = (perms[0] == 'r' ? PROT_READ : 0) | (perms[1] == 'w' ? PROT_WRITE : 0) |
               (perms[2] == 'x' ? PROT_EXEC : 0);
      v.shared = perms[3] == 's';
      v.inode = inode;
      if (name_at > 0 && (size_t)name_at < line.size()) v.path = line.substr(name_at);
      out.push_back(std::move(v));
    } else if (line.starts_with("VmFlags:") && !out.empty()) {
      out.back().flags = line.substr(9);
    }
  }
  return out;
}

uint64_t ArgStart(pid_t pid) {
  Str stat = ReadFile(f("/proc/{}/stat", pid));
  StrView rest = StrView(stat).substr(stat.rfind(')') + 2);
  for (int field = 3; field < 48 && !rest.empty(); ++field) rest.remove_prefix(rest.find(' ') + 1);
  return strtoull(Str(rest.substr(0, rest.find(' '))).c_str(), nullptr, 10);
}

bool Mirrorable(const Vma& v, uint64_t arg_start) {
  if (v.path == "[vdso]" || v.path == "[vvar]" || v.path == "[vsyscall]" ||
      v.path == "[vvar_vclock]")
    return false;
  if (v.HasFlag("io") || v.HasFlag("pf") || v.HasFlag("dc") || v.HasFlag("wf")) return false;
  if (v.HasFlag("gd") && v.path != "[stack]") return false;
  if (!v.inode && v.start <= arg_start && arg_start < v.end) return false;
  return v.prot != PROT_NONE;
}

struct Resources {
  int fds = 0;
  int threads = 0, mirror_mappings = 0, reservations = 0;
  uint64_t heap = 0;

  static Resources Now() {
    Resources r;
    for (int fd : DirNumbers("/proc/self/fd")) {
      char link[256];
      ssize_t n = readlink(f("/proc/self/fd/{}", fd).c_str(), link, sizeof link);
      StrView target(link, n > 0 ? n : 0);
      r.fds += target.contains("memfd:pm-") || target == "anon_inode:[pidfd]";
    }
    r.threads = DirNumbers("/proc/self/task").size();
    for (auto& v : ReadMaps("/proc/self/maps")) {
      if (v.path.find("memfd:pm-") != Str::npos) ++r.mirror_mappings;
      if (v.prot == PROT_NONE && v.end - v.start >= kGiB) ++r.reservations;
    }
    r.heap = mallinfo2().uordblks;
    return r;
  }
  Str ToStr() const {
    return f(
        "memfd and pidfd descriptors {}, threads {}, memfd mappings {}, local mirror reservations "
        "{}, heap {:.2f} MiB",
        fds, threads, mirror_mappings, reservations, MiB(heap));
  }
};

Vec<pid_t> Descendants(pid_t pid) {
  Vec<pid_t> out;
  for (int tid : DirNumbers(f("/proc/{}/task", pid))) {
    Str children = ReadFile(f("/proc/{}/task/{}/children", pid, tid));
    for (char* p = children.data(); *p;) {
      char* end;
      long child = strtol(p, &end, 10);
      if (end == p) break;
      out.push_back(child);
      for (pid_t d : Descendants(child)) out.push_back(d);
      p = end;
    }
  }
  return out;
}

struct Target {
  pid_t pid = 0;
  int pidfd = -1;
  int to = -1, from = -1;
  Str buffer;

  void Spawn() {
    Path path = Path::ExecutablePath().Parent() / "mem_meld_target";
    int in_pipe[2], out_pipe[2];
    ASSERT_EQ(pipe2(in_pipe, O_CLOEXEC), 0);
    ASSERT_EQ(pipe2(out_pipe, O_CLOEXEC), 0);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, in_pipe[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO);
    char* argv[] = {const_cast<char*>(path.c_str()), nullptr};
    int err = posix_spawn(&pid, path.c_str(), &actions, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(in_pipe[0]);
    close(out_pipe[1]);
    to = in_pipe[1];
    from = out_pipe[0];
    if (err) pid = 0;
    ASSERT_EQ(err, 0) << strerror(err);
    pidfd = (int)syscall(SYS_pidfd_open, pid, 0);
    ASSERT_GE(pidfd, 0);
    Str line;
    ASSERT_TRUE(ReadLine(line, 10000));
    ASSERT_TRUE(line.starts_with("pid ")) << line;
  }

  bool Send(StrView text) {
    for (size_t done = 0; done < text.size();) {
      ssize_t n = write(to, text.data() + done, text.size() - done);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) return false;
      done += n;
    }
    return true;
  }

  bool ReadLine(Str& line, int timeout_ms) {
    auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
      if (size_t nl = buffer.find('\n'); nl != Str::npos) {
        line = buffer.substr(0, nl);
        buffer.erase(0, nl + 1);
        return true;
      }
      int left = (int)Ms(deadline - Clock::now());
      if (left <= 0) return false;
      pollfd p{from, POLLIN, 0};
      if (poll(&p, 1, left) <= 0) continue;
      char chunk[4096];
      ssize_t n = read(from, chunk, sizeof chunk);
      if (n <= 0) return false;
      buffer.append(chunk, n);
    }
  }

  Str Command(StrView line, int timeout_ms = 60000) {
    Str reply;
    if (!Send(Str(line) + "\n") || !ReadLine(reply, timeout_ms)) return "(no reply)";
    return reply;
  }

  uintptr_t CommandAddr(StrView line) {
    Str reply = Command(line);
    EXPECT_TRUE(reply.starts_with("ok ")) << line << " -> " << reply;
    return reply.starts_with("ok ") ? strtoull(reply.c_str() + 3, nullptr, 16) : 0;
  }

  Vec<Str> Batch(const Vec<Str>& lines, Vec<Clock::time_point>* times = nullptr,
                 int timeout_ms = 60000) {
    Str text;
    for (auto& line : lines) text += line + "\n";
    Vec<Str> replies;
    if (!Send(text)) return replies;
    for (size_t i = 0; i < lines.size(); ++i) {
      Str reply;
      if (!ReadLine(reply, timeout_ms)) break;
      replies.push_back(std::move(reply));
      if (times) times->push_back(Clock::now());
    }
    return replies;
  }

  void Kill() {
    if (pid <= 0) return;
    for (pid_t d : Descendants(pid)) kill(d, SIGKILL);
    syscall(SYS_pidfd_send_signal, pidfd, SIGKILL, nullptr, 0);
  }

  void Finish() {
    if (pid <= 0) return;
    Kill();
    siginfo_t info;
    while (waitid(P_PIDFD, pidfd, &info, WEXITED) < 0 && errno == EINTR) {
    }
    close(pidfd);
    close(to);
    close(from);
    pid = 0;
    pidfd = to = from = -1;
  }

  void Abandon() {
    Kill();
    close(pidfd);
    close(to);
    close(from);
    pid = 0;
    pidfd = to = from = -1;
  }

  ~Target() { Finish(); }
};

std::atomic<int> late_handler_calls = 0;

struct StressMirror {
  using Region = MemMeld::Region;

  struct Rec {
    Span<char> local;
    int prot;
    Str path;
  };
  struct Event {
    uintptr_t remote;
    uint64_t size;
    Clock::time_point at;
  };

  std::mutex mutex;
  std::condition_variable cv;
  std::map<uintptr_t, Rec> live;
  uint64_t live_bytes = 0;
  bool attached = false;
  Clock::time_point attached_at;
  int detached_calls = 0;
  bool detached_ok = false;
  Str detached_status;
  uint64_t events = 0;
  uint64_t max_mirror = 0;
  uint64_t live_at_max_mirror = 0;
  Vec<Str> violations;
  bool log_mapped = false;
  Vec<Event> mapped_log;
  std::map<uintptr_t, Clock::time_point> mapped_at, unmapped_at;
  std::atomic<bool> detach_returned = false;
  MemMeld mem_meld;
  std::thread thread;

  StressMirror() {
    mem_meld.on_attached = [this] { OnAttached(); };
    mem_meld.on_mapped = [this](const Region& r) { OnMapped(r); };
    mem_meld.on_changed = [this](const Region& before, const Region& after) {
      OnChanged(before, after);
    };
    mem_meld.on_unmapped = [this](const Region& r) { OnUnmapped(r); };
    mem_meld.on_detached = [this](const Status& status) { OnDetached(status); };
  }

  void Attach(int pidfd) {
    int own = fcntl(pidfd, F_DUPFD_CLOEXEC, 0);
    thread = std::thread([this, own] {
      SetThreadName("MemMeld");
      Status status;
      mem_meld.Attach(own, status);
      close(own);
      if (!OK(status)) OnDetached(status);
    });
  }

  void Detach() {
    mem_meld.Detach();
    if (thread.joinable()) thread.join();
  }

  bool Late() {
    if (!detach_returned.load(std::memory_order_relaxed)) return false;
    late_handler_calls.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  void Violation(Str text) {
    if (violations.size() < 20) violations.push_back(std::move(text));
  }

  void CheckNew(const Region& r, StrView event) {
    const Region* found = mem_meld.Find(r.remote);
    if (!found || found->remote != r.remote || found->local.data() != r.local.data() ||
        found->local.size() != r.local.size())
      Violation(f("{}: Find({:#x}) disagrees with the event", event, r.remote));
    if (mem_meld.ToRemote(r.local.data()) != r.remote)
      Violation(f("{}: ToRemote of {:#x} is wrong", event, r.remote));
    Span<char> local_mirror = mem_meld.Mirror();
    if (r.local.data() < local_mirror.data() ||
        r.local.data() + r.local.size() > local_mirror.data() + local_mirror.size())
      Violation(f("{}: {:#x} lies outside Mirror()", event, r.remote));
    auto next = live.lower_bound(r.remote);
    if (next != live.end() && next->first < r.remote + r.local.size())
      Violation(f("{}: [{:#x}, +{:#x}) overlaps live {:#x}", event, r.remote, r.local.size(),
                  next->first));
    if (next != live.begin()) {
      auto prev = std::prev(next);
      if (prev->first + prev->second.local.size() > r.remote)
        Violation(f("{}: [{:#x}, +{:#x}) overlaps live {:#x}", event, r.remote, r.local.size(),
                    prev->first));
    }
  }

  void Remove(const Region& r, StrView event) {
    auto it = live.find(r.remote);
    if (it == live.end() || it->second.local.data() != r.local.data() ||
        it->second.local.size() != r.local.size()) {
      Violation(f("{}: {:#x} is not the live region", event, r.remote));
      return;
    }
    live_bytes -= it->second.local.size();
    live.erase(it);
  }

  void Add(const Region& r) {
    live[r.remote] = {r.local, r.prot, Str(r.path)};
    live_bytes += r.local.size();
  }

  void Counted() {
    ++events;
    uint64_t local_mirror = mem_meld.Mirror().size();
    if (local_mirror > max_mirror) {
      max_mirror = local_mirror;
      live_at_max_mirror = live_bytes;
    }
    cv.notify_all();
  }

  void OnAttached() {
    if (Late()) return;
    std::lock_guard lock(mutex);
    attached = true;
    attached_at = Clock::now();
    cv.notify_all();
  }
  void OnMapped(const Region& r) {
    if (Late()) return;
    auto now = Clock::now();
    std::lock_guard lock(mutex);
    CheckNew(r, "OnMapped");
    Add(r);
    mapped_at[r.remote] = now;
    if (log_mapped) mapped_log.push_back({r.remote, r.local.size(), now});
    Counted();
  }
  void OnChanged(const Region& before, const Region& after) {
    if (Late()) return;
    std::lock_guard lock(mutex);
    Remove(before, "OnChanged");
    mapped_at.erase(before.remote);
    CheckNew(after, "OnChanged");
    Add(after);
    mapped_at[after.remote] = Clock::now();
    Counted();
  }
  void OnUnmapped(const Region& r) {
    if (Late()) return;
    std::lock_guard lock(mutex);
    Remove(r, "OnUnmapped");
    mapped_at.erase(r.remote);
    unmapped_at[r.remote] = Clock::now();
    Counted();
  }
  void OnDetached(const Status& status) {
    if (Late()) return;
    std::lock_guard lock(mutex);
    ++detached_calls;
    detached_ok = OK(status);
    detached_status = status.ToStr();
    cv.notify_all();
  }

  uint64_t Events() {
    std::lock_guard lock(mutex);
    return events;
  }
  bool WaitAttached(Clock::duration timeout) {
    std::unique_lock lock(mutex);
    return cv.wait_for(lock, timeout, [&] { return attached || detached_calls; }) && attached;
  }
  bool WaitDetached(Clock::duration timeout) {
    std::unique_lock lock(mutex);
    return cv.wait_for(lock, timeout, [&] { return detached_calls > 0; });
  }
  std::optional<Clock::time_point> WaitMapped(uintptr_t remote, uint64_t len,
                                              Clock::time_point after, Clock::duration timeout) {
    std::unique_lock lock(mutex);
    auto covering = [&] {
      auto it = live.upper_bound(remote);
      if (it == live.begin()) return live.end();
      --it;
      return remote + len <= it->first + it->second.local.size() ? it : live.end();
    };
    auto ready = [&] {
      auto it = covering();
      return it != live.end() && mapped_at.contains(it->first) && mapped_at[it->first] >= after;
    };
    if (!cv.wait_for(lock, timeout, ready)) return {};
    return mapped_at[covering()->first];
  }
  std::optional<Clock::time_point> WaitUnmapped(uintptr_t remote, Clock::time_point after,
                                                Clock::duration timeout) {
    std::unique_lock lock(mutex);
    auto ready = [&] { return unmapped_at.contains(remote) && unmapped_at[remote] >= after; };
    if (!cv.wait_for(lock, timeout, ready)) return {};
    return unmapped_at[remote];
  }
};

void StartAttach(StressMirror& m, Target& t) {
  int pidfd = (int)syscall(SYS_pidfd_open, t.pid, 0);
  m.Attach(pidfd);
  close(pidfd);
}

std::unique_ptr<StressMirror> Attach(Target& t, double* seconds = nullptr) {
  auto m = std::make_unique<StressMirror>();
  auto start = Clock::now();
  StartAttach(*m, t);
  if (!m->WaitAttached(120s)) {
    ADD_FAILURE() << "attach failed: " << m->detached_status;
    return nullptr;
  }
  if (seconds) *seconds = Seconds(m->attached_at - start);
  return m;
}

void Destroy(std::unique_ptr<StressMirror>& m) {
  if (m) {
    m->Detach();
    m->detach_returned.store(true, std::memory_order_relaxed);
  }
  m.reset();
}

bool IsGuardPage(int pagemap, uint64_t address) {
  uint64_t entry = 0;
  return pread(pagemap, &entry, sizeof entry, address / kPage * 8) == (ssize_t)sizeof entry &&
         (entry & kPagemapGuardRegion);
}

uint64_t CompareBytes(pid_t pid, const std::map<uintptr_t, StressMirror::Rec>& live,
                      const std::function<void(const Str&)>& problem) {
  int pagemap = open(f("/proc/{}/pagemap", pid).c_str(), O_RDONLY | O_CLOEXEC);
  if (pagemap < 0) {
    problem("cannot open the target's pagemap");
    return 0;
  }
  uint64_t compared = 0;
  Vec<uint64_t> entries;
  Vec<unsigned char> resident;
  Vec<char> remote_bytes;
  for (auto& [remote, rec] : live) {
    if (rec.prot == PROT_NONE) continue;
    uint64_t pages = rec.local.size() / kPage;
    entries.resize(pages);
    resident.resize(pages);
    if (pread(pagemap, entries.data(), pages * 8, remote / kPage * 8) != (ssize_t)(pages * 8) ||
        mincore(rec.local.data(), rec.local.size(), resident.data()) != 0) {
      problem(f("cannot read the page state of {:#x}", remote));
      continue;
    }
    for (uint64_t k = 0; k < pages; ++k)
      if (entries[k] & kPagemapGuardRegion)
        problem(f("the region {:#x} ({}) mirrors the guard page at {:#x}", remote, rec.path,
                  remote + k * kPage));
    auto wanted = [&](uint64_t k) { return (entries[k] >> 62) != 0 || (resident[k] & 1); };
    for (uint64_t i = 0; i < pages;) {
      if (!wanted(i)) {
        ++i;
        continue;
      }
      uint64_t j = i;
      while (j < pages && j - i < 256 && wanted(j)) ++j;
      uint64_t len = (j - i) * kPage;
      remote_bytes.resize(len);
      iovec l{remote_bytes.data(), len}, r{(void*)(remote + i * kPage), len};
      const char* local = rec.local.data() + i * kPage;
      if (process_vm_readv(pid, &l, 1, &r, 1, 0) != (ssize_t)len) {
        problem(f("cannot read the target at {:#x} ({})", remote + i * kPage, rec.path));
      } else if (memcmp(remote_bytes.data(), local, len) != 0) {
        uint64_t k = 0;
        while (remote_bytes[k] == local[k]) ++k;
        problem(f("bytes differ at {:#x} ({}, prot {}): target {:02x}, mirror {:02x}",
                  remote + i * kPage + k, rec.path, rec.prot, (uint8_t)remote_bytes[k],
                  (uint8_t)local[k]));
      }
      compared += j - i;
      i = j;
    }
  }
  close(pagemap);
  return compared;
}

uint64_t ExpectConsistent(pid_t pid, StressMirror& m, const Str& when) {
  int reported = 0;
  auto problem = [&](const Str& text) {
    if (reported++ < 10) ADD_FAILURE() << when << ": " << text;
  };
  std::map<uintptr_t, StressMirror::Rec> live;
  {
    std::lock_guard lock(m.mutex);
    live = m.live;
    for (auto& v : m.violations) problem("handler check: " + v);
    m.violations.clear();
  }

  Span<char> local_mirror;
  Vec<Str> library = [&] {
    Vec<Str> out;
    local_mirror = m.mem_meld.Mirror();
    for (auto& [remote, rec] : live) {
      uint64_t size = rec.local.size();
      const MemMeld::Region* r = m.mem_meld.Find(remote + size / 2);
      if (!r || r->remote != remote || r->local.data() != rec.local.data() ||
          r->local.size() != size || r->prot != rec.prot) {
        out.push_back(f("Find inside [{:#x}, +{:#x}) disagrees with the events", remote, size));
        continue;
      }
      if (m.mem_meld.ToRemote(rec.local.data() + size - 1) != remote + size - 1)
        out.push_back(f("ToRemote of the last byte of {:#x} is wrong", remote));
      Span<char> tail = m.mem_meld.ToLocal(remote + size - 1, 1);
      if (tail.data() != rec.local.data() + size - 1 || tail.size() != 1)
        out.push_back(f("ToLocal of the last byte of {:#x} is wrong", remote));
      if (rec.local.data() < local_mirror.data() ||
          rec.local.data() + size > local_mirror.data() + local_mirror.size())
        out.push_back(f("{:#x} lies outside Mirror()", remote));
    }
    return out;
  }();
  for (auto& text : library) problem(text);

  Vec<std::pair<const char*, uint64_t>> locals;
  for (auto& [remote, rec] : live) locals.push_back({rec.local.data(), rec.local.size()});
  std::sort(locals.begin(), locals.end());
  for (size_t i = 1; i < locals.size(); ++i)
    if (locals[i - 1].first + locals[i - 1].second > locals[i].first)
      problem("two regions share local bytes");

  bool mirror_readable = true;
  {
    uint64_t cursor = (uint64_t)local_mirror.data(), end = cursor + local_mirror.size();
    for (auto& v : ReadMaps("/proc/self/maps")) {
      if (v.end <= cursor || cursor >= end) continue;
      if (v.start > cursor || !(v.prot & PROT_READ)) break;
      cursor = v.end;
    }
    if (cursor < end) {
      mirror_readable = false;
      problem(f("Mirror() is not readable at offset {:#x} of {:#x}",
                cursor - (uint64_t)local_mirror.data(), local_mirror.size()));
    }
  }

  Vec<Vma> vmas = ReadMaps(f("/proc/{}/smaps", pid));
  for (auto& [remote, rec] : live) {
    uint64_t cursor = remote, end = remote + rec.local.size();
    auto it = std::upper_bound(vmas.begin(), vmas.end(), cursor,
                               [](uint64_t a, const Vma& v) { return a < v.end; });
    for (; cursor < end; ++it) {
      if (it == vmas.end() || it->start > cursor) {
        problem(
            f("mirrored [{:#x}, {:#x}) ({}) is not mapped in the target", cursor, end, rec.path));
        break;
      }
      if (it->prot != rec.prot)
        problem(f("[{:#x}, {:#x}) has protection {} in the target and {} in the mirror",
                  std::max(cursor, it->start), std::min(end, it->end), it->prot, rec.prot));
      cursor = it->end;
    }
  }
  auto covered_until = [&](uint64_t cursor) {
    auto it = live.upper_bound(cursor);
    if (it == live.begin()) return cursor;
    --it;
    uint64_t end = it->first + it->second.local.size();
    return end > cursor ? end : cursor;
  };
  uint64_t arg_start = ArgStart(pid);
  FD pagemap = open(f("/proc/{}/pagemap", pid).c_str(), O_RDONLY | O_CLOEXEC);
  for (auto& v : vmas) {
    bool memfd = v.path.find("memfd:pm-mirror") != Str::npos && v.prot != PROT_NONE;
    bool anonymous = !v.shared && v.inode == 0 && (v.prot & PROT_WRITE) && Mirrorable(v, arg_start);
    if (!memfd && !anonymous) continue;
    for (uint64_t cursor = v.start; cursor < v.end;) {
      uint64_t next = covered_until(cursor);
      if (next == cursor && IsGuardPage(pagemap, cursor)) next = cursor + kPage;
      if (next == cursor) {
        problem(f("{} [{:#x}, {:#x}) {} is not mirrored at {:#x}",
                  memfd ? "memfd mapping" : "writable anonymous mapping", v.start, v.end, v.path,
                  cursor));
        break;
      }
      cursor = next;
    }
  }

  return mirror_readable ? CompareBytes(pid, live, problem) : 0;
}

struct Owned {
  uintptr_t addr;
  uint64_t len;
  uint64_t seed;
  Clock::time_point mapped_after;
};

Owned MapOwned(Target& t, uint64_t len, uint64_t seed) {
  auto sent = Clock::now();
  uintptr_t addr = t.CommandAddr(f("mmap {} rw", len));
  EXPECT_EQ(t.Command(f("write {:#x} {} {}", addr, len, seed)), "ok");
  return {addr, len, seed, sent};
}

Span<char> OwnedBytes(StressMirror& m, const Owned& o) {
  if (!m.WaitMapped(o.addr, o.len, o.mapped_after, 30s)) return {};
  return m.mem_meld.ToLocal(o.addr, o.len);
}

Str MirrorHolds(pid_t pid, StressMirror& m, const Owned& o) {
  Span<char> local = OwnedBytes(m, o);
  if (local.size() < o.len) return f("{:#x}: ToLocal returns {:#x} bytes", o.addr, local.size());
  for (uint64_t i = 0; i < o.len; ++i) {
    if ((uint8_t)local[i] == Pattern(o.seed, i)) continue;
    uint8_t remote = 0;
    iovec l{&remote, 1}, r{(void*)(o.addr + i), 1};
    process_vm_readv(pid, &l, 1, &r, 1, 0);
    return f("{:#x}+{:#x}: mirror {:02x}, target {:02x}, pattern {:02x}", o.addr, i,
             (uint8_t)local[i], remote, Pattern(o.seed, i));
  }
  return {};
}

void WriteThroughMirror(StressMirror& m, const Owned& o) {
  Span<char> local = OwnedBytes(m, o);
  ASSERT_GE(local.size(), o.len);
  for (uint64_t i = 0; i < o.len; ++i) local[i] = (char)Pattern(o.seed, i);
}

void ExpectTargetWorks(Target& t, const Str& when) {
  uintptr_t a = t.CommandAddr("mmap 65536 rw");
  EXPECT_EQ(t.Command(f("write {:#x} 65536 77", a)), "ok") << when;
  uintptr_t b = t.CommandAddr(f("mremap {:#x} 65536 131072", a));
  EXPECT_EQ(t.Command(f("verify {:#x} 65536 77", b)), "ok") << when;
  EXPECT_EQ(t.Command(f("madvise {:#x} 131072 dontneed", b)), "ok") << when;
  EXPECT_TRUE(t.Command(f("verify {:#x} 4096 77", b)).starts_with("mismatch")) << when;
  EXPECT_EQ(t.Command(f("munmap {:#x} 131072", b)), "ok") << when;
  EXPECT_EQ(t.Command("fork"), "ok") << when;
  Str forktest = t.Command("forktest");
  EXPECT_TRUE(forktest.starts_with("ok mutex=ok parent=")) << when << ": " << forktest;
  size_t parent = forktest.find("parent="), child = forktest.find("child=");
  if (parent != Str::npos && child != Str::npos)
    EXPECT_NE(forktest.substr(parent + 7, 32), forktest.substr(child + 6, 32)) << when;
  EXPECT_EQ(t.Command("forkexec"), "ok status=0") << when;
  uintptr_t big = t.CommandAddr("malloc 1000000");
  EXPECT_EQ(t.Command(f("free {:#x}", big)), "ok") << when;
  EXPECT_EQ(t.Command("malloc_trim"), "ok") << when;
}

Vec<Str> ExistingLibraries() {
  Vec<Str> out;
  for (const char* name :
       {"libm.so.6", "libz.so.1", "libresolv.so.2", "libutil.so.1", "libdl.so.2", "librt.so.1",
        "libanl.so.1", "libbz2.so.1.0", "liblzma.so.5", "libzstd.so.1", "libexpat.so.1",
        "libuuid.so.1", "libffi.so.8", "libgmp.so.10", "libmd.so.0", "libtinfo.so.6",
        "libattr.so.1", "libacl.so.1", "libgcc_s.so.1"}) {
    if (void* library = dlopen(name, RTLD_LAZY)) {
      dlclose(library);
      out.push_back(name);
    }
  }
  return out;
}

enum class Load { kMaps, kDlopen, kForks, kMallocs, kMixed };

struct Stream {
  Target& target;
  Load load;
  std::atomic<bool> stop = false;
  uint64_t commands = 0;
  Vec<Str> errors;
  bool target_gone = false;
  std::thread thread;

  Stream(Target& target, Load load) : target(target), load(load) {
    thread = std::thread([this] { Run(); });
  }
  ~Stream() { Finish(); }

  void Finish() {
    stop.store(true, std::memory_order_relaxed);
    if (thread.joinable()) thread.join();
  }

  bool Exchange(const Vec<Str>& lines, Vec<Str>& follow) {
    Vec<Str> replies = target.Batch(lines);
    if (replies.size() != lines.size()) {
      target_gone = true;
      return false;
    }
    commands += lines.size();
    for (size_t i = 0; i < lines.size(); ++i) {
      if (!replies[i].starts_with("ok")) {
        if (errors.size() < 5) errors.push_back(lines[i] + " -> " + replies[i]);
        continue;
      }
      if (lines[i].starts_with("mmap "))
        follow.push_back(f("munmap {} 8192", replies[i].substr(3)));
      if (lines[i].starts_with("malloc ")) follow.push_back(f("free {}", replies[i].substr(3)));
    }
    return true;
  }

  void Run() {
    Vec<Str> libraries = ExistingLibraries();
    for (uint64_t round = 0; !stop.load(std::memory_order_relaxed); ++round) {
      Vec<Str> lines;
      auto add = [&](int count, auto make) {
        for (int i = 0; i < count; ++i) lines.push_back(make(i));
      };
      auto mmap = [](int) { return Str("mmap 8192 rw"); };
      auto malloc = [&](int i) { return f("malloc {}", 16 + (round * 31 + i * 97) % 4000); };
      auto dlopen = [&](int i) {
        return f("dlopen {}", libraries[(round * 8 + i) % libraries.size()]);
      };
      auto forks = [](int i) { return Str(i % 2 ? "forkexec" : "fork"); };
      switch (load) {
        case Load::kMaps:
          add(16, mmap);
          break;
        case Load::kDlopen:
          add(8, dlopen);
          break;
        case Load::kForks:
          add(2, forks);
          break;
        case Load::kMallocs:
          add(32, malloc);
          break;
        case Load::kMixed:
          add(4, dlopen);
          add(2, forks);
          add(8, mmap);
          add(8, malloc);
          break;
      }
      Vec<Str> follow, none;
      if (!Exchange(lines, follow) || (!follow.empty() && !Exchange(follow, none))) return;
    }
  }
};

struct SignalStorm {
  std::atomic<bool> stop = false;
  uint64_t sent = 0;
  std::thread thread;

  SignalStorm(pid_t pid, int pidfd) {
    thread = std::thread([this, pid, pidfd] {
      const int signals[] = {SIGWINCH, SIGURG, SIGCHLD};
      for (uint64_t i = 0; !stop.load(std::memory_order_relaxed); ++i) {
        int sig = signals[i % 3];
        if (i % 2) {
          syscall(SYS_pidfd_send_signal, pidfd, sig, nullptr, 0);
        } else if (Vec<int> tids = DirNumbers(f("/proc/{}/task", pid)); !tids.empty()) {
          syscall(SYS_tgkill, pid, tids[i / 2 % tids.size()], sig);
        }
        ++sent;
        std::this_thread::sleep_for(1ms);
      }
    });
  }
  ~SignalStorm() {
    stop.store(true, std::memory_order_relaxed);
    thread.join();
  }
};

struct AttachMeter {
  pid_t pid;
  std::atomic<bool> stop = false;
  double longest_stop = 0;
  double stop_span = 0;
  uint64_t peak_bytes = 0;
  std::thread thread;

  explicit AttachMeter(pid_t pid) : pid(pid) {
    thread = std::thread([this] {
      std::map<int, Clock::time_point> stopped_since;
      std::optional<Clock::time_point> first, last;
      auto next_memory = Clock::now();
      while (!stop.load(std::memory_order_relaxed)) {
        auto now = Clock::now();
        for (int tid : DirNumbers(f("/proc/{}/task", this->pid))) {
          Str stat = ReadFile(f("/proc/{}/task/{}/stat", this->pid, tid));
          size_t paren = stat.rfind(')');
          if (paren != Str::npos && paren + 2 < stat.size() && stat[paren + 2] == 't') {
            if (!first) first = now;
            last = now;
            auto since = stopped_since.try_emplace(tid, now).first->second;
            longest_stop = std::max(longest_stop, Seconds(now - since));
          } else {
            stopped_since.erase(tid);
          }
        }
        if (now >= next_memory) {
          peak_bytes = std::max(peak_bytes, Memory::Of(this->pid).rss_anon + MirrorMemfdBytes());
          next_memory = now + 1ms;
        }
      }
      if (first) stop_span = Seconds(*last - *first);
    });
  }
  void Finish() {
    stop.store(true, std::memory_order_relaxed);
    if (thread.joinable()) thread.join();
  }
  ~AttachMeter() { Finish(); }
};

Str Percentiles(Vec<double> values) {
  if (values.empty()) return "none";
  std::sort(values.begin(), values.end());
  auto at = [&](double q) {
    return values[std::min(values.size() - 1, (size_t)(q * values.size()))];
  };
  return f("p50 {:.3f} ms, p99 {:.3f} ms, max {:.3f} ms (n={})", at(0.5), at(0.99), values.back(),
           values.size());
}

struct Workload {
  struct Region {
    uintptr_t addr;
    uint64_t len;
    bool whole;
    bool writable;
  };

  Target& t;
  std::mt19937_64 rng;
  Vec<Region> regions;
  uint64_t ops = 0;
  Vec<Str> errors;
  Vec<StressMirror::Event> mapped;

  Workload(Target& t, uint64_t seed) : t(t), rng(seed) {}

  void Error(const Str& line, const Str& reply) {
    if (errors.size() < 5) errors.push_back(line + " -> " + reply);
  }

  bool Round() {
    Vec<Str> lines;
    Vec<uint64_t> lens;
    size_t maps = regions.size() < 2000 ? 128 : 32;
    for (size_t i = 0; i < maps; ++i) {
      lens.push_back((1 + rng() % 16) * kPage);
      lines.push_back(f("mmap {} rw", lens.back()));
    }
    Vec<Clock::time_point> times;
    Vec<Str> replies = t.Batch(lines, &times);
    if (replies.size() != lines.size()) return false;
    for (size_t i = 0; i < maps; ++i) {
      if (!replies[i].starts_with("ok ")) {
        Error(lines[i], replies[i]);
        continue;
      }
      uintptr_t addr = strtoull(replies[i].c_str() + 3, nullptr, 16);
      regions.push_back({addr, lens[i], true, true});
      mapped.push_back({addr, lens[i], times[i]});
    }
    ops += maps;

    enum Kind { kUnmap, kPunch, kResize, kDontneed, kProtectTail, kWrite };
    struct Op {
      size_t index;
      Kind kind;
      uint64_t arg;
    };
    Vec<Op> todo;
    lines.clear();
    size_t count = std::min<size_t>(regions.size(), 160);
    Vec<size_t> order(regions.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    for (size_t i = 0; i < count; ++i) std::swap(order[i], order[i + rng() % (order.size() - i)]);
    for (size_t i = 0; i < count; ++i) {
      Region& r = regions[order[i]];
      uint64_t pages = r.len / kPage;
      Op op{order[i], kDontneed, 0};
      switch (rng() % 10) {
        case 0:
        case 1:
        case 2:
        case 3:
          op.kind = kUnmap;
          break;
        case 4:
          if (pages >= 3) op = {order[i], kPunch, 1 + rng() % (pages - 2)};
          break;
        case 5:
          if (r.whole) op = {order[i], kResize, r.len + (1 + rng() % 4) * kPage};
          break;
        case 6:
          if (r.whole && pages >= 2) op = {order[i], kResize, (1 + rng() % (pages - 1)) * kPage};
          break;
        case 7:
          break;
        case 8:
          if (pages >= 2) op.kind = kProtectTail;
          break;
        case 9:
          if (r.writable) op = {order[i], kWrite, rng()};
          break;
      }
      switch (op.kind) {
        case kUnmap:
          lines.push_back(f("munmap {:#x} {}", r.addr, r.len));
          break;
        case kPunch:
          lines.push_back(f("munmap {:#x} {}", r.addr + op.arg * kPage, kPage));
          break;
        case kResize:
          lines.push_back(f("mremap {:#x} {} {}", r.addr, r.len, op.arg));
          break;
        case kDontneed:
          lines.push_back(f("madvise {:#x} {} dontneed", r.addr, r.len));
          break;
        case kProtectTail:
          lines.push_back(f("mprotect {:#x} {} r", r.addr + r.len - kPage, kPage));
          break;
        case kWrite:
          lines.push_back(f("write {:#x} {} {}", r.addr, r.len, op.arg));
          break;
      }
      todo.push_back(op);
    }
    replies = t.Batch(lines);
    if (replies.size() != lines.size()) return false;
    ops += lines.size();
    Vec<bool> dead(regions.size());
    for (size_t i = 0; i < todo.size(); ++i) {
      if (!replies[i].starts_with("ok")) {
        Error(lines[i], replies[i]);
        continue;
      }
      Region& r = regions[todo[i].index];
      switch (todo[i].kind) {
        case kUnmap:
          dead[todo[i].index] = true;
          break;
        case kPunch: {
          uint64_t cut = todo[i].arg * kPage;
          regions.push_back({r.addr + cut + kPage, r.len - cut - kPage, false, r.writable});
          regions[todo[i].index].len = cut;
          regions[todo[i].index].whole = false;
          break;
        }
        case kResize:
          r.addr = strtoull(replies[i].c_str() + 3, nullptr, 16);
          r.len = todo[i].arg;
          break;
        case kProtectTail:
          regions.push_back({r.addr + r.len - kPage, kPage, false, false});
          regions[todo[i].index].len -= kPage;
          regions[todo[i].index].whole = false;
          break;
        case kDontneed:
        case kWrite:
          break;
      }
    }
    dead.resize(regions.size());
    size_t kept = 0;
    for (size_t i = 0; i < regions.size(); ++i)
      if (!dead[i]) regions[kept++] = regions[i];
    regions.resize(kept);
    return true;
  }
};

struct Stress : ::testing::Test {
  static void SetUpTestSuite() {
    static bool initialized = false;
    if (initialized) return;
    initialized = true;
    signal(SIGPIPE, SIG_IGN);
    Report(f("seed {} seconds {} rounds {} threads {} mappings {} large {} MiB", kSeed, kSeconds,
             kRounds, kThreads, kMappings, kLargeMiB));
  }
};

TEST_F(Stress, ChurningThreadsStayConsistent) {
  Target t;
  ASSERT_NO_FATAL_FAILURE(t.Spawn());
  auto m = Attach(t);
  ASSERT_TRUE(m);
  std::mt19937_64 rng(kSeed ^ 1);
  Vec<Owned> owned;
  for (int i = 0; i < 24; ++i) owned.push_back(MapOwned(t, (1 + rng() % 16) * kPage, rng()));
  int cycles = 0, live_checks = 0;
  uint64_t compared = 0;
  auto start = Clock::now();
  while (Clock::now() < start + std::chrono::seconds(kSeconds)) {
    ASSERT_EQ(t.Command(f("threads {}", kThreads)), "ok");
    for (auto until = Clock::now() + 300ms; Clock::now() < until; ++live_checks) {
      Owned& a = owned[rng() % owned.size()];
      a.seed = rng();
      ASSERT_EQ(t.Command(f("write {:#x} {} {}", a.addr, a.len, a.seed)), "ok");
      EXPECT_EQ(MirrorHolds(t.pid, *m, a), "");
      Owned& b = owned[rng() % owned.size()];
      b.seed = rng();
      WriteThroughMirror(*m, b);
      EXPECT_EQ(t.Command(f("verify {:#x} {} {}", b.addr, b.len, b.seed)), "ok");
      Owned& c = owned[rng() % owned.size()];
      EXPECT_EQ(t.Command(f("munmap {:#x} {}", c.addr, c.len)), "ok");
      c = MapOwned(t, (1 + rng() % 16) * kPage, rng());
    }
    ASSERT_EQ(t.Command("threads_stop"), "ok");
    compared += ExpectConsistent(t.pid, *m, f("after churn cycle {}", cycles));
    for (auto& o : owned) {
      EXPECT_EQ(t.Command(f("verify {:#x} {} {}", o.addr, o.len, o.seed)), "ok");
      EXPECT_EQ(MirrorHolds(t.pid, *m, o), "");
    }
    ++cycles;
    if (HasFailure()) break;
  }
  Report(f(
      "churn: {} cycles of {} threads, {} live checks, {} events, {} pages compared, local mirror "
      "max {:.1f} MiB at {:.1f} MiB live, {} target VMAs at the end",
      cycles, kThreads, live_checks, m->Events(), compared, MiB(m->max_mirror),
      MiB(m->live_at_max_mirror), ReadMaps(f("/proc/{}/maps", t.pid)).size()));
  Destroy(m);
}

TEST_F(Stress, HighEventRate) {
  uint64_t target_ops = 20000 * kSeconds;
  uint64_t seed = kSeed ^ 2;
  double baseline_s, baseline_cpu;
  {
    Target t;
    ASSERT_NO_FATAL_FAILURE(t.Spawn());
    Workload w(t, seed);
    uint64_t cpu = CpuNs(t.pid, t.pid);
    auto start = Clock::now();
    while (w.ops < target_ops) ASSERT_TRUE(w.Round());
    baseline_s = Seconds(Clock::now() - start);
    baseline_cpu = (CpuNs(t.pid, t.pid) - cpu) * 1e-9;
    EXPECT_TRUE(w.errors.empty()) << w.errors[0];
  }

  Target t;
  ASSERT_NO_FATAL_FAILURE(t.Spawn());
  auto m = Attach(t);
  ASSERT_TRUE(m);
  Vec<double> unloaded;
  for (int i = 0; i < 100; ++i) {
    auto sent = Clock::now();
    uintptr_t a = t.CommandAddr("mmap 4096 rw");
    auto at = m->WaitMapped(a, kPage, sent, 10s);
    ASSERT_TRUE(at);
    unloaded.push_back(Ms(*at - sent));
    ASSERT_EQ(t.Command(f("munmap {:#x} 4096", a)), "ok");
  }
  {
    std::lock_guard lock(m->mutex);
    m->log_mapped = true;
    m->max_mirror = 0;
  }
  pid_t tracer_tid = TracerTid();
  uint64_t tracer_cpu = CpuNs(getpid(), tracer_tid), events = m->Events();
  Workload w(t, seed);
  uint64_t cpu = CpuNs(t.pid, t.pid);
  auto start = Clock::now();
  Vec<std::tuple<double, uint64_t, uint64_t>> mirror_series;
  auto next_sample = start;
  while (w.ops < target_ops) {
    ASSERT_TRUE(w.Round());
    if (Clock::now() >= next_sample) {
      uint64_t local_mirror = m->mem_meld.Mirror().size();
      std::lock_guard lock(m->mutex);
      mirror_series.push_back({Seconds(Clock::now() - start), local_mirror, m->live_bytes});
      next_sample += 250ms;
    }
  }
  double mirrored_s = Seconds(Clock::now() - start);
  double mirrored_cpu = (CpuNs(t.pid, t.pid) - cpu) * 1e-9;
  double tracer_s = (CpuNs(getpid(), tracer_tid) - tracer_cpu) * 1e-9;
  uint64_t workload_events = m->Events() - events;
  EXPECT_TRUE(w.errors.empty()) << w.errors[0];
  uint64_t compared = ExpectConsistent(t.pid, *m, "after the event storm");

  Vec<double> lag;
  {
    std::lock_guard lock(m->mutex);
    std::map<std::pair<uintptr_t, uint64_t>, Vec<Clock::time_point>> replies;
    for (auto& e : w.mapped) replies[{e.remote, e.size}].push_back(e.at);
    std::map<std::pair<uintptr_t, uint64_t>, size_t> used;
    for (auto& e : m->mapped_log) {
      auto key = std::make_pair(e.remote, e.size);
      auto it = replies.find(key);
      if (it == replies.end() || used[key] >= it->second.size()) continue;
      lag.push_back(Ms(e.at - it->second[used[key]++]));
    }
  }
  uint64_t mirror_now = m->mem_meld.Mirror().size();
  Str series;
  for (size_t i = 0; i < mirror_series.size(); i += std::max<size_t>(1, mirror_series.size() / 8)) {
    auto [s, local_mirror, live] = mirror_series[i];
    series += f(" {:.1f}s:{:.1f}/{:.1f}", s, MiB(local_mirror), MiB(live));
  }
  Report(
      f("event rate: {} ops; baseline {:.0f} ops/s ({:.2f} us target CPU/op); mirrored {:.0f} "
        "ops/s ({:.2f} us target CPU/op); slowdown {:.2f}x wall, {:.2f}x target CPU",
        w.ops, w.ops / baseline_s, baseline_cpu * 1e6 / w.ops, w.ops / mirrored_s,
        mirrored_cpu * 1e6 / w.ops, mirrored_s / baseline_s, mirrored_cpu / baseline_cpu));
  Report(
      f("event rate: {} events ({:.0f}/s); tracer thread CPU {:.1f}% ({:.2f} us/event); {} "
        "pages compared",
        workload_events, workload_events / mirrored_s, 100 * tracer_s / mirrored_s,
        tracer_s * 1e6 / workload_events, compared));
  Report(f("event rate: unloaded mmap->OnMapped {}", Percentiles(unloaded)));
  Report(f("event rate: loaded OnMapped minus reply {}", Percentiles(lag)));
  Report(f(
      "event rate: local mirror max {:.1f} MiB at {:.1f} MiB live; final {:.1f} MiB local mirror, "
      "{:.1f} "
      "MiB live; local mirror/live MiB over time:{}",
      MiB(m->max_mirror), MiB(m->live_at_max_mirror), MiB(mirror_now), MiB(m->live_bytes), series));
  Destroy(m);
}

TEST_F(Stress, SystemCallCost) {
  const uint64_t getpids = 50000 * kSeconds, mmaps = 10000 * kSeconds;
  auto measure = [&](bool mirrored) {
    Target t;
    t.Spawn();
    std::unique_ptr<StressMirror> m = mirrored ? Attach(t) : nullptr;
    auto start = Clock::now();
    EXPECT_EQ(t.Command(f("getpids {}", getpids)), "ok");
    auto middle = Clock::now();
    EXPECT_EQ(t.Command(f("mmaps {}", mmaps)), "ok");
    auto end = Clock::now();
    Destroy(m);
    return std::pair(Seconds(middle - start) * 1e6 / getpids, Seconds(end - middle) * 1e6 / mmaps);
  };
  auto [plain_getpid, plain_mmap] = measure(false);
  auto [mirrored_getpid, mirrored_mmap] = measure(true);
  Report(
      f("system calls: getpid {:.2f} us unmirrored, {:.2f} us mirrored ({:.0f}x); mmap and munmap "
        "{:.2f} us unmirrored, {:.2f} us mirrored ({:.0f}x)",
        plain_getpid, mirrored_getpid, mirrored_getpid / plain_getpid, plain_mmap, mirrored_mmap,
        mirrored_mmap / plain_mmap));
}

TEST_F(Stress, LargeRegionsStaySparse) {
  uint64_t dense = kLargeMiB * kMiB, sparse = 16 * dense, step = 64 * kMiB;
  Target t;
  ASSERT_NO_FATAL_FAILURE(t.Spawn());
  uintptr_t d = t.CommandAddr(f("mmap {} rw", dense));
  ASSERT_EQ(t.Command(f("write {:#x} {} 1", d, dense), 600000), "ok");
  uintptr_t s = t.CommandAddr(f("mmap {} rw", sparse));
  Vec<Str> touches;
  for (uint64_t o = 0; o < sparse; o += step)
    touches.push_back(f("write {:#x} {} {}", s + o, kPage, 100 + o / step));
  for (auto& reply : t.Batch(touches)) ASSERT_EQ(reply, "ok");
  uint64_t touched = touches.size() * kPage;
  Memory target_before = Memory::Of(t.pid), self_before = Memory::Of(getpid());

  AttachMeter meter(t.pid);
  double attach_s;
  auto m = Attach(t, &attach_s);
  meter.Finish();
  ASSERT_TRUE(m);
  Memory target_after = Memory::Of(t.pid), self_after = Memory::Of(getpid());
  uint64_t memfd = MirrorMemfdBytes();
  EXPECT_LT(memfd, target_before.rss_anon + target_before.rss_shmem + 64 * kMiB);
  EXPECT_LT(memfd, sparse / 2) << "sparse memory became dense";
  EXPECT_LT(target_after.rss_anon + target_after.rss_shmem,
            target_before.rss_anon + target_before.rss_shmem + 128 * kMiB);

  auto check_start = Clock::now();
  uint64_t compared = ExpectConsistent(t.pid, *m, "large regions");
  double check_s = Seconds(Clock::now() - check_start);
  uint64_t memfd_after_read = MirrorMemfdBytes();
  Memory self_after_read = Memory::Of(getpid());
  EXPECT_LT(memfd_after_read, memfd + 16 * kMiB) << "reading the mirror allocated memory";

  auto big_sent = Clock::now();
  uintptr_t big = t.CommandAddr(f("mmap {} rw", dense));
  auto big_replied = Clock::now();
  auto big_mapped = m->WaitMapped(big, dense, big_sent, 120s);
  ASSERT_TRUE(big_mapped);
  EXPECT_EQ(t.Command(f("munmap {:#x} {}", big, dense)), "ok");
  auto big_unmap_replied = Clock::now();
  auto big_unmapped = m->WaitUnmapped(big, big_replied, 120s);
  ASSERT_TRUE(big_unmapped);

  uint64_t reserve = 64 * kGiB;
  uintptr_t r = t.CommandAddr(f("mmap {} none", reserve));
  auto sent = Clock::now();
  EXPECT_EQ(t.Command(f("munmap {:#x} {}", r, reserve)), "ok");
  uintptr_t probe = t.CommandAddr("mmap 4096 rw");
  auto probe_mapped = m->WaitMapped(probe, kPage, sent, 600s);
  ASSERT_TRUE(probe_mapped);

  Report(f("large: dense {:.0f} MiB written, sparse {:.0f} MiB with {} touched pages", MiB(dense),
           MiB(sparse), touches.size()));
  Report(
      f("large: attach {:.3f} s; longest thread stop {:.3f} s, stop span {:.3f} s; peak target "
        "anonymous + memfd {:.1f} MiB",
        attach_s, meter.longest_stop, meter.stop_span, MiB(meter.peak_bytes)));
  Report(
      f("large: target before attach {}; after {}", target_before.ToStr(), target_after.ToStr()));
  Report(f("large: Automat before attach {}; after {}; after reading every page {}",
           self_before.ToStr(), self_after.ToStr(), self_after_read.ToStr()));
  Report(
      f("large: memfd allocated {:.1f} MiB after attach, {:.1f} MiB after the full check "
        "({:.3f} s, {} pages)",
        MiB(memfd), MiB(memfd_after_read), check_s, compared));
  Report(
      f("large: a new {:.0f} MiB region reaches OnMapped {:.3f} ms after the reply, OnUnmapped "
        "{:.3f} ms after the munmap reply",
        MiB(dense), Ms(*big_mapped - big_replied), Ms(*big_unmapped - big_unmap_replied)));
  Report(
      f("large: after unmapping a {} GiB PROT_NONE reservation the next event arrives after "
        "{:.1f} ms",
        reserve / kGiB, Ms(*probe_mapped - sent)));
  Destroy(m);
}

Vec<uintptr_t> MapPages(Target& t, uint64_t count) {
  Vec<uintptr_t> addrs;
  for (uint64_t done = 0; done < count; done += 500) {
    Vec<Str> lines;
    for (uint64_t i = done; i < std::min(count, done + 500); ++i)
      lines.push_back(i % 2 ? "mmap 4096 r" : "mmap 4096 rw");
    for (auto& reply : t.Batch(lines)) {
      EXPECT_TRUE(reply.starts_with("ok ")) << reply;
      addrs.push_back(strtoull(reply.c_str() + 3, nullptr, 16));
    }
  }
  return addrs;
}

TEST_F(Stress, ThousandsOfMappingsAtAttach) {
  Target t;
  ASSERT_NO_FATAL_FAILURE(t.Spawn());
  Vec<uintptr_t> addrs = MapPages(t, kMappings);
  ASSERT_EQ(addrs.size(), kMappings);
  Vec<Str> writes;
  for (size_t i = 0; i < addrs.size(); i += 2)
    writes.push_back(f("write {:#x} 4096 {}", addrs[i], i));
  for (size_t i = 0; i < writes.size(); i += 500) {
    Vec<Str> part(writes.begin() + i, writes.begin() + std::min(writes.size(), i + 500));
    for (auto& reply : t.Batch(part)) ASSERT_EQ(reply, "ok");
  }
  size_t vmas = ReadMaps(f("/proc/{}/maps", t.pid)).size();
  double attach_s;
  auto m = Attach(t, &attach_s);
  ASSERT_TRUE(m);
  size_t at_attach = m->Events();
  EXPECT_GE(at_attach, kMappings);
  auto check_start = Clock::now();
  ExpectConsistent(t.pid, *m, "thousands of mappings");
  double check_s = Seconds(Clock::now() - check_start);
  for (size_t i = 0; i < addrs.size(); i += 2)
    EXPECT_EQ(MirrorHolds(t.pid, *m, {addrs[i], kPage, i, {}}), "");

  auto start = Clock::now();
  for (size_t done = 0; done < addrs.size(); done += 500) {
    Vec<Str> lines;
    for (size_t i = done; i < std::min(addrs.size(), done + 500); ++i)
      lines.push_back(f("munmap {:#x} 4096", addrs[i]));
    for (auto& reply : t.Batch(lines)) ASSERT_EQ(reply, "ok");
  }
  double unmap_s = Seconds(Clock::now() - start);
  ExpectConsistent(t.pid, *m, "after unmapping them");
  Report(
      f("mappings: {} target VMAs, {} regions at attach, attach {:.3f} s ({:.1f} us per "
        "region), full check {:.3f} s, {} unmaps applied in {:.3f} s",
        vmas, at_attach, attach_s, attach_s * 1e6 / at_attach, check_s, addrs.size(), unmap_s));
  Destroy(m);
}

struct BusyCase {
  const char* name;
  bool threads;
  std::optional<Load> load;
  bool signals;
};

void AttachWhileBusy(const Vec<BusyCase>& cases, uint64_t rounds) {
  for (auto& c : cases) {
    Vec<double> times;
    for (uint64_t round = 0; round < rounds; ++round) {
      Target t;
      ASSERT_NO_FATAL_FAILURE(t.Spawn());
      if (c.threads) ASSERT_EQ(t.Command("threads 16"), "ok");
      std::optional<Stream> stream;
      if (c.load) stream.emplace(t, *c.load);
      std::optional<SignalStorm> storm;
      if (c.signals) storm.emplace(t.pid, t.pidfd);
      std::this_thread::sleep_for(20ms);
      double attach_s;
      auto m = Attach(t, &attach_s);
      std::this_thread::sleep_for(50ms);
      storm.reset();
      if (stream) {
        stream->Finish();
        EXPECT_FALSE(stream->target_gone) << c.name;
        EXPECT_TRUE(stream->errors.empty()) << c.name << ": " << stream->errors[0];
      }
      ASSERT_TRUE(m) << c.name;
      times.push_back(attach_s * 1000);
      if (c.threads) ASSERT_EQ(t.Command("threads_stop"), "ok");
      Str when = f("{} round {}", c.name, round);
      ExpectConsistent(t.pid, *m, when);
      ExpectTargetWorks(t, when);
      ExpectConsistent(t.pid, *m, when + " after more work");
      Destroy(m);
      if (::testing::Test::HasFailure()) return;
    }
    Report(f("busy attach, {}: attach {}", c.name, Percentiles(times)));
  }
}

TEST_F(Stress, AttachWhileTheTargetIsBusy) {
  AttachWhileBusy({{"malloc threads", true, std::nullopt, false},
                   {"malloc loop", false, Load::kMallocs, false},
                   {"dlopen loop", false, Load::kDlopen, false},
                   {"fork loop", false, Load::kForks, false},
                   {"signals", true, std::nullopt, true}},
                  std::max<uint64_t>(1, kRounds / 4));
}

TEST_F(Stress, AttachWhileEverythingRuns) {
  AttachWhileBusy({{"everything", true, Load::kMixed, true}}, 4);
}

TEST_F(Stress, ForkLoopsKeepTheParentIntact) {
  auto fork_loop = [](Target& t, uint64_t forks) {
    const char* commands[] = {"fork", "forkexec", "forktest"};
    for (uint64_t i = 0; i < forks; ++i) {
      Str reply = t.Command(commands[i % 3], 10000);
      if (i % 3 == 2)
        EXPECT_TRUE(reply.starts_with("ok mutex=ok")) << i << ": " << reply;
      else
        EXPECT_TRUE(reply == "ok" || reply == "ok status=0") << i << ": " << reply;
      if (reply == "(no reply)") return;
    }
  };
  uint64_t forks = kRounds * 20;
  double baseline_s;
  {
    Target t;
    ASSERT_NO_FATAL_FAILURE(t.Spawn());
    auto start = Clock::now();
    fork_loop(t, forks);
    baseline_s = Seconds(Clock::now() - start);
  }
  Target t;
  ASSERT_NO_FATAL_FAILURE(t.Spawn());
  auto m = Attach(t);
  ASSERT_TRUE(m);
  std::mt19937_64 rng(kSeed ^ 3);
  Vec<Owned> owned;
  for (int i = 0; i < 8; ++i) owned.push_back(MapOwned(t, (1 + rng() % 64) * kPage, rng()));
  ASSERT_EQ(t.Command("threads 8"), "ok");
  ASSERT_EQ(t.Command("threads_stop"), "ok");
  size_t fds = DirNumbers(f("/proc/{}/fd", t.pid)).size();
  size_t vmas = ReadMaps(f("/proc/{}/maps", t.pid)).size();
  uint64_t memfd = MirrorMemfdBytes();
  ASSERT_EQ(t.Command("threads 8"), "ok");
  auto start = Clock::now();
  fork_loop(t, forks);
  double mirrored_s = Seconds(Clock::now() - start);
  ASSERT_EQ(t.Command("threads_stop"), "ok");
  EXPECT_EQ(DirNumbers(f("/proc/{}/fd", t.pid)).size(), fds);
  size_t vmas_after = ReadMaps(f("/proc/{}/maps", t.pid)).size();
  uint64_t memfd_after = MirrorMemfdBytes();
  EXPECT_LT(memfd_after, memfd + 32 * kMiB);
  for (auto& o : owned) {
    EXPECT_EQ(t.Command(f("verify {:#x} {} {}", o.addr, o.len, o.seed)), "ok");
    EXPECT_EQ(MirrorHolds(t.pid, *m, o), "");
  }
  ExpectConsistent(t.pid, *m, "after the fork loop");
  Report(
      f("forks: {} forks/forkexecs/forktests; {:.2f} ms each without the mirror, {:.2f} ms with "
        "it; target VMAs {} -> {}, memfd {:.1f} -> {:.1f} MiB",
        forks, baseline_s * 1000 / forks, mirrored_s * 1000 / forks, vmas, vmas_after, MiB(memfd),
        MiB(memfd_after)));
  Destroy(m);
}

TEST_F(Stress, RepeatedAttachAndDestroy) {
  auto round = [](uint64_t r) {
    Target t;
    ASSERT_NO_FATAL_FAILURE(t.Spawn());
    auto m = Attach(t);
    ASSERT_TRUE(m);
    Owned o = MapOwned(t, 16 * kPage, r);
    ASSERT_EQ(MirrorHolds(t.pid, *m, o), "");
    Destroy(m);
    if (r % 2) {
      Stream stream(t, Load::kMixed);
      std::this_thread::sleep_for(20ms);
      stream.Finish();
      EXPECT_FALSE(stream.target_gone);
      EXPECT_TRUE(stream.errors.empty()) << stream.errors[0];
    }
    EXPECT_EQ(t.Command(f("verify {:#x} {} {}", o.addr, o.len, o.seed)), "ok");
    ExpectTargetWorks(t, f("round {} after the mirror went away", r));
    for (auto& v : ReadMaps(f("/proc/{}/maps", t.pid)))
      EXPECT_EQ(v.path.find("memfd:pm-mirror"), Str::npos) << "round " << r;
  };
  round(0);
  Resources before = Resources::Now();
  uint64_t rounds = kRounds * 2;
  auto start = Clock::now();
  for (uint64_t r = 1; r <= rounds && !HasFailure(); ++r) round(r);
  double seconds = Seconds(Clock::now() - start);
  Resources after = Resources::Now();
  EXPECT_EQ(after.fds, before.fds);
  EXPECT_EQ(after.threads, before.threads);
  EXPECT_EQ(after.mirror_mappings, before.mirror_mappings);
  EXPECT_EQ(after.reservations, before.reservations);
  EXPECT_EQ(late_handler_calls.load(), 0);
  Report(
      f("attach/destroy: {} rounds in {:.2f} s; before {}; after {}; heap growth {:.1f} KiB per "
        "round",
        rounds, seconds, before.ToStr(), after.ToStr(),
        ((double)after.heap - (double)before.heap) / 1024 / rounds));
}

void KillAtRandomMoments(bool during_attach_rounds, uint64_t pages = 0) {
  double typical_attach_s = 0;
  {
    Target t;
    ASSERT_NO_FATAL_FAILURE(t.Spawn());
    MapPages(t, pages);
    auto m = Attach(t, &typical_attach_s);
    ASSERT_TRUE(m);
    Destroy(m);
  }
  Resources before = Resources::Now();
  std::mt19937_64 rng(kSeed ^ 4);
  std::map<Str, int> outcomes;
  uint64_t rounds = kRounds * 2;
  for (uint64_t r = 0; r < rounds && !::testing::Test::HasFailure(); ++r) {
    Target t;
    ASSERT_NO_FATAL_FAILURE(t.Spawn());
    MapPages(t, pages);
    bool busy = r % 2;
    bool during_attach = during_attach_rounds && r % 4 < 2;
    if (busy) ASSERT_EQ(t.Command("threads 8"), "ok");
    std::optional<Stream> stream;
    if (busy) stream.emplace(t, Load::kMaps);
    auto m = std::make_unique<StressMirror>();
    StartAttach(*m, t);
    if (during_attach) {
      std::uniform_real_distribution<double> delay(0, 1.5 * typical_attach_s);
      std::this_thread::sleep_for(std::chrono::duration<double>(delay(rng)));
    } else {
      ASSERT_TRUE(m->WaitAttached(60s)) << m->detached_status;
      std::this_thread::sleep_for(std::chrono::milliseconds(rng() % 30));
    }
    t.Kill();
    if (!m->WaitDetached(30s)) {
      ADD_FAILURE() << f("no OnDetached in round {} ({}, {})", r, busy ? "busy" : "idle",
                         during_attach ? "during the attach" : "after OnAttached");
      (void)m.release();
      if (stream) stream->Finish();
      t.Abandon();
      return;
    }
    std::this_thread::sleep_for(20ms);
    {
      std::lock_guard lock(m->mutex);
      EXPECT_EQ(m->detached_calls, 1) << r;
      Str kind = m->attached ? "after OnAttached" : "before OnAttached";
      ++outcomes[f("{} ({}): {}", kind, busy ? "busy" : "idle",
                   m->detached_ok ? Str("OK") : m->detached_status)];
    }
    if (stream) stream->Finish();
    Destroy(m);
    t.Finish();
  }
  std::this_thread::sleep_for(50ms);
  Resources after = Resources::Now();
  EXPECT_EQ(after.fds, before.fds);
  EXPECT_EQ(after.threads, before.threads);
  EXPECT_EQ(after.mirror_mappings, before.mirror_mappings);
  EXPECT_EQ(after.reservations, before.reservations);
  EXPECT_EQ(waitpid(-1, nullptr, WNOHANG), -1) << "a target was left unreaped";
  EXPECT_EQ(late_handler_calls.load(), 0);
  for (auto& [outcome, count] : outcomes) Report(f("kill: {} x {}", count, outcome));
  Report(f("kill: {} rounds; before {}; after {}", rounds, before.ToStr(), after.ToStr()));
}

TEST_F(Stress, KillingTheTargetDetachesOnce) { KillAtRandomMoments(false); }

TEST_F(Stress, KillingTheTargetDuringAttach) { KillAtRandomMoments(true); }

TEST_F(Stress, KillingTheTargetDuringItsConversion) { KillAtRandomMoments(true, 8 * kMappings); }

char State(pid_t pid) {
  Str stat = ReadFile(f("/proc/{}/stat", pid));
  size_t paren = stat.rfind(')');
  return paren != Str::npos && paren + 2 < stat.size() ? stat[paren + 2] : '?';
}

TEST_F(Stress, KillingTheTargetInsideAnInjectedCall) {
  uint64_t len = 4 * kLargeMiB * kMiB;
  for (int round = 0; round < 3; ++round) {
    Target t;
    ASSERT_NO_FATAL_FAILURE(t.Spawn());
    uintptr_t a = t.CommandAddr(f("mmap {} rw", len));
    ASSERT_EQ(t.Command(f("madvise {:#x} {} nohugepage", a, len)), "ok");
    ASSERT_EQ(t.Command(f("write {:#x} {} 1", a, len), 600000), "ok");
    auto m = std::make_unique<StressMirror>();
    StartAttach(*m, t);
    bool copied = false;
    char state = 0;
    auto since = Clock::now(), deadline = since + 60s;
    while (Clock::now() < deadline) {
      char current = State(t.pid);
      if ((current == 't') != (state == 't')) since = Clock::now();
      state = current;
      copied |= state == 't' && Clock::now() - since > 100ms;
      if (copied && state != 't' && Clock::now() - since > 2ms) break;
      std::this_thread::sleep_for(20us);
    }
    t.Kill();
    ASSERT_TRUE(m->WaitDetached(60s));
    Destroy(m);
    siginfo_t exit = {};
    EXPECT_EQ(waitid(P_PIDFD, t.pidfd, &exit, WEXITED | WNOWAIT), 0) << "round " << round;
  }
}

bool TargetZero(pid_t pid, uintptr_t addr, uint64_t len) {
  Vec<char> bytes(len);
  iovec l{bytes.data(), len}, r{(void*)addr, len};
  if (process_vm_readv(pid, &l, 1, &r, 1, 0) != (ssize_t)len) return false;
  return std::all_of(bytes.begin(), bytes.end(), [](char c) { return c == 0; });
}

TEST_F(Stress, MunmapReleasesMemfdPages) {
  Target t;
  ASSERT_NO_FATAL_FAILURE(t.Spawn());
  auto m = Attach(t);
  ASSERT_TRUE(m);
  uintptr_t a = t.CommandAddr("mmap 67108864 rw");
  ASSERT_EQ(t.Command(f("write {:#x} 67108864 5", a)), "ok");
  uint64_t before = MirrorMemfdBytes();
  ASSERT_EQ(t.Command(f("munmap {:#x} 67108864", a)), "ok");
  EXPECT_LT(MirrorMemfdBytes(), before - 60 * kMiB);
  Destroy(m);
}

TEST_F(Stress, ShrunkThenRegrownMemoryReadsZero) {
  Target t;
  ASSERT_NO_FATAL_FAILURE(t.Spawn());
  auto m = Attach(t);
  ASSERT_TRUE(m);
  uintptr_t a = t.CommandAddr("mmap 65536 rw");
  ASSERT_EQ(t.Command(f("write {:#x} 65536 9", a)), "ok");
  uintptr_t b = t.CommandAddr(f("mremap {:#x} 65536 16384", a));
  uintptr_t c = t.CommandAddr(f("mremap {:#x} 16384 65536", b));
  EXPECT_TRUE(TargetZero(t.pid, c + 16384, 49152));
  Destroy(m);
}

TEST_F(Stress, CallocAfterMallocTrimReadsZero) {
  Target t;
  ASSERT_NO_FATAL_FAILURE(t.Spawn());
  auto m = Attach(t);
  ASSERT_TRUE(m);
  Vec<uintptr_t> blocks;
  for (int i = 0; i < 30; ++i) {
    blocks.push_back(t.CommandAddr("malloc 100000"));
    ASSERT_EQ(t.Command(f("write {:#x} 100000 {}", blocks.back(), i + 1)), "ok");
  }
  for (uintptr_t b : blocks) ASSERT_EQ(t.Command(f("free {:#x}", b)), "ok");
  ASSERT_EQ(t.Command("malloc_trim"), "ok");
  for (int i = 0; i < 30; ++i) {
    uintptr_t c = t.CommandAddr("calloc 1 100000");
    EXPECT_TRUE(TargetZero(t.pid, c, 100000)) << f("calloc {} at {:#x}", i, c);
  }
  Destroy(m);
}

TEST_F(Stress, MapFixedOverMirroredMemoryReleasesIt) {
  Target t;
  ASSERT_NO_FATAL_FAILURE(t.Spawn());
  auto m = Attach(t);
  ASSERT_TRUE(m);
  uintptr_t a = t.CommandAddr("mmap 67108864 rw");
  ASSERT_EQ(t.Command(f("write {:#x} 67108864 5", a)), "ok");
  uint64_t before = MirrorMemfdBytes();
  ASSERT_EQ(t.CommandAddr(f("mmap 67108864 rw {:#x}", a)), a);
  EXPECT_LT(MirrorMemfdBytes(), before - 60 * kMiB);
  Destroy(m);
}

TEST_F(Stress, RegionLargerThanASlotIsNotShared) {
  Target t;
  ASSERT_NO_FATAL_FAILURE(t.Spawn());
  auto m = Attach(t);
  ASSERT_TRUE(m);
  uintptr_t a = t.CommandAddr(f("mmap {} rw", 80 * kGiB));
  uintptr_t b = t.CommandAddr("mmap 4096 rw");
  ASSERT_EQ(t.Command(f("write {:#x} 4096 1", b)), "ok");
  ASSERT_EQ(t.Command(f("write {:#x} 4096 2", a + 64 * kGiB)), "ok");
  EXPECT_EQ(t.Command(f("verify {:#x} 4096 1", b)), "ok");
  Destroy(m);
}

void SurvivesInChild(const std::function<void()>& scenario) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_EXIT(
      {
        scenario();
        _exit(::testing::Test::HasFailure() ? 1 : 0);
      },
      ::testing::ExitedWithCode(0), "");
}

TEST_F(Stress, DestroyWhileTheTargetIsActive) {
  SurvivesInChild([] {
    for (int r = 0; r < 40; ++r) {
      Target t;
      t.Spawn();
      auto m = Attach(t);
      if (!m) return;
      Stream stream(t, Load::kMaps);
      std::this_thread::sleep_for(std::chrono::milliseconds(r % 5));
      Destroy(m);
      stream.Finish();
    }
  });
}

TEST_F(Stress, DestroyDuringAttach) {
  SurvivesInChild([] {
    auto round = [] {
      Target t;
      t.Spawn();
      auto m = std::make_unique<StressMirror>();
      StartAttach(*m, t);
      Destroy(m);
      std::this_thread::sleep_for(100ms);
    };
    round();
    Resources before = Resources::Now();
    for (int r = 0; r < 20; ++r) round();
    Resources after = Resources::Now();
    EXPECT_EQ(late_handler_calls.load(), 0);
    EXPECT_EQ(after.reservations, before.reservations) << after.ToStr();
    EXPECT_EQ(after.fds, before.fds) << after.ToStr();
  });
}

TEST_F(Stress, UnmapOfAHugeReservationIsCheap) {
  Target t;
  ASSERT_NO_FATAL_FAILURE(t.Spawn());
  auto m = Attach(t);
  ASSERT_TRUE(m);
  uint64_t reserve = 4ull << 40;
  uintptr_t r = t.CommandAddr(f("mmap {} none", reserve));
  auto sent = Clock::now();
  ASSERT_EQ(t.Command(f("munmap {:#x} {}", r, reserve)), "ok");
  uintptr_t probe = t.CommandAddr("mmap 4096 rw");
  auto at = m->WaitMapped(probe, kPage, sent, 600s);
  ASSERT_TRUE(at);
  EXPECT_LT(Ms(*at - sent), 50.0) << "the tracer thread stalled";
  Destroy(m);
}

TEST_F(Stress, MremapOfASubRange) {
  Target t;
  ASSERT_NO_FATAL_FAILURE(t.Spawn());
  auto m = Attach(t);
  ASSERT_TRUE(m);
  uintptr_t a = t.CommandAddr("mmap 12288 rw");
  ASSERT_EQ(t.Command(f("write {:#x} 12288 3", a)), "ok");
  t.CommandAddr(f("mremapmove {:#x} 4096 4096", a + 4096));
  ExpectConsistent(t.pid, *m, "after moving the middle page");
  Destroy(m);
}

TEST_F(Stress, MremapOfARegionSplitAndRejoined) {
  Target t;
  ASSERT_NO_FATAL_FAILURE(t.Spawn());
  auto m = Attach(t);
  ASSERT_TRUE(m);
  uintptr_t a = t.CommandAddr("mmap 12288 rw");
  ASSERT_EQ(t.Command(f("mprotect {:#x} 4096 r", a + 4096)), "ok");
  ASSERT_EQ(t.Command(f("mprotect {:#x} 4096 rw", a + 4096)), "ok");
  ASSERT_EQ(t.Command(f("write {:#x} 12288 3", a)), "ok");
  t.CommandAddr(f("mremapmove {:#x} 12288 24576", a));
  ExpectConsistent(t.pid, *m, "after moving the rejoined region");
  Destroy(m);
}

}  // namespace
