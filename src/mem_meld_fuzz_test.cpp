// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <thread>

#include "fd.hpp"
#include "format.hpp"
#include "gtest.hpp"
#include "mem_meld.hpp"
#include "path.hpp"
#include "thread_name.hpp"
#include "tracy_client.hpp"
#include "vec.hpp"

using namespace automat;
using namespace std::chrono_literals;

namespace {

constexpr uint64_t kPage = 4096;
constexpr uint64_t kPagemapGuardRegion = 1ull << 58;
constexpr int kA = 0;
constexpr int kB = 1;
constexpr uint64_t kFilePages = 64;
constexpr int kProts[] = {PROT_NONE, PROT_READ, PROT_READ | PROT_WRITE, PROT_READ | PROT_EXEC,
                          PROT_READ | PROT_WRITE | PROT_EXEC};
constexpr int kFileProts[] = {PROT_READ, PROT_READ | PROT_WRITE, PROT_READ | PROT_EXEC};
constexpr const char* kLibraries[] = {"libm.so.6", "libz.so.1", "libbz2.so.1", "liblzma.so.5",
                                      "libuuid.so.1"};

uint8_t PatternByte(uint64_t seed, uint64_t index) {
  uint64_t x = seed * 6364136223846793005ull + index * 1442695040888963407ull + 1;
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdull;
  x ^= x >> 33;
  return (uint8_t)x;
}

void Print(StrView text) {
  while (!text.empty()) {
    ssize_t n = write(STDERR_FILENO, text.data(), text.size());
    if (n <= 0) return;
    text.remove_prefix(n);
  }
}

uint64_t Env(const char* name, uint64_t fallback) {
  const char* value = getenv(name);
  return value && *value ? strtoull(value, nullptr, 0) : fallback;
}

Str ReadFile(const Str& path) {
  Str out;
  int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return out;
  char buf[65536];
  for (ssize_t n; (n = read(fd, buf, sizeof buf)) > 0;) out.append(buf, n);
  close(fd);
  return out;
}

struct Rng {
  uint64_t state;
  uint64_t Next() {
    uint64_t z = (state += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
  }
};

const char* ProtName(int prot) {
  switch (prot) {
    case PROT_NONE:
      return "none";
    case PROT_READ:
      return "r";
    case PROT_READ | PROT_WRITE:
      return "rw";
    case PROT_READ | PROT_EXEC:
      return "rx";
    default:
      return "rwx";
  }
}

struct Vma {
  uint64_t start = 0, end = 0, offset = 0, inode = 0;
  int prot = 0;
  bool shared = false;
  Str path, flags;

  bool HasFlag(const char* flag) const {
    return (" " + flags + " ").find(f(" {} ", flag)) != Str::npos;
  }
};

Vec<Vma> ReadVmas(const Str& file) {
  Vec<Vma> out;
  Str text = ReadFile(file);
  for (size_t pos = 0; pos < text.size();) {
    size_t nl = text.find('\n', pos);
    if (nl == Str::npos) nl = text.size();
    Str line = text.substr(pos, nl - pos);
    pos = nl + 1;
    if (line.starts_with("VmFlags:")) {
      if (!out.empty()) out.back().flags = line.substr(8);
      continue;
    }
    Vma v;
    char perms[8] = {};
    unsigned major, minor;
    int name = 0;
    if (sscanf(line.c_str(), "%lx-%lx %7s %lx %x:%x %lu %n", &v.start, &v.end, perms, &v.offset,
               &major, &minor, &v.inode, &name) != 7)
      continue;
    v.prot = (perms[0] == 'r' ? PROT_READ : 0) | (perms[1] == 'w' ? PROT_WRITE : 0) |
             (perms[2] == 'x' ? PROT_EXEC : 0);
    v.shared = perms[3] == 's';
    if (name > 0 && (size_t)name < line.size()) v.path = line.substr(name);
    out.push_back(std::move(v));
  }
  return out;
}

const Vma* Covering(const Vec<Vma>& vmas, uint64_t addr) {
  auto it = std::upper_bound(vmas.begin(), vmas.end(), addr,
                             [](uint64_t a, const Vma& v) { return a < v.start; });
  if (it == vmas.begin()) return nullptr;
  --it;
  return addr < it->end ? &*it : nullptr;
}

Str DescribeVma(const Vma& v) {
  return f("[{:#x}, {:#x}) {}{} offset {:#x} '{}'{}", v.start, v.end, ProtName(v.prot),
           v.shared ? "s" : "p", v.offset, v.path, v.flags);
}

template <typename Fn>
void ForEachData(int fd, uint64_t lo, uint64_t hi, Fn&& fn) {
  for (uint64_t at = lo; at < hi;) {
    off_t data = lseek(fd, at, SEEK_DATA);
    if (data < 0 || (uint64_t)data >= hi) return;
    off_t hole = lseek(fd, data, SEEK_HOLE);
    uint64_t end = hole < 0 ? hi : std::min<uint64_t>(hole, hi);
    fn((uint64_t)data, end - data);
    at = end;
  }
}

struct MappedFile {
  int fd = -1;
  Str path;
  Str maps_path = "/memfd:pm-fuzz-file (deleted)";

  MappedFile() {
    fd = memfd_create("pm-fuzz-file", MFD_CLOEXEC);
    Vec<uint8_t> bytes(kFilePages * kPage);
    for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = PatternByte(777, i);
    (void)!pwrite(fd, bytes.data(), bytes.size(), 0);
    path = f("/proc/{}/fd/{}", getpid(), fd);
  }
};

MappedFile& File() {
  static MappedFile file;
  return file;
}

struct Helper {
  pid_t pid = 0;
  int to = -1, from = -1, mem = -1;
  bool hung = false;
  Str buffer;

  Str Spawn(bool randomize) {
    Path path = Path::ExecutablePath().Parent() / "mem_meld_target";
    int in_pipe[2], out_pipe[2];
    if (pipe2(in_pipe, O_CLOEXEC) || pipe2(out_pipe, O_CLOEXEC)) return "pipe2";
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, in_pipe[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO);
    char* argv[] = {const_cast<char*>(path.c_str()), nullptr};
    int persona = personality(0xffffffff);
    if (!randomize) personality(persona | ADDR_NO_RANDOMIZE);
    int err = posix_spawn(&pid, path.c_str(), &actions, nullptr, argv, environ);
    personality(persona);
    posix_spawn_file_actions_destroy(&actions);
    close(in_pipe[0]);
    close(out_pipe[1]);
    to = in_pipe[1];
    from = out_pipe[0];
    if (err) {
      pid = 0;
      return f("posix_spawn: {}", strerror(err));
    }
    Str line;
    if (!ReadLine(line) || !line.starts_with("pid ")) return "the helper did not start";
    mem = open(f("/proc/{}/mem", pid).c_str(), O_RDONLY | O_CLOEXEC);
    return {};
  }

  bool Read(uint64_t addr, void* buf, size_t len) const {
    iovec local{buf, len}, remote{(void*)addr, len};
    if (process_vm_readv(pid, &local, 1, &remote, 1, 0) == (ssize_t)len) return true;
    return pread(mem, buf, len, addr) == (ssize_t)len;
  }

  bool ReadLine(Str& line) {
    auto deadline = std::chrono::steady_clock::now() + 20s;
    for (;;) {
      if (size_t nl = buffer.find('\n'); nl != Str::npos) {
        line = buffer.substr(0, nl);
        buffer.erase(0, nl + 1);
        return true;
      }
      auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now());
      if (left.count() <= 0) {
        hung = true;
        return false;
      }
      pollfd pfd{from, POLLIN, 0};
      int r = poll(&pfd, 1, (int)left.count());
      if (r < 0 && errno == EINTR) continue;
      if (r == 0) {
        hung = true;
        return false;
      }
      char chunk[4096];
      ssize_t n = read(from, chunk, sizeof chunk);
      if (n <= 0) return false;
      buffer.append(chunk, n);
    }
  }

  bool Command(StrView line, Str& reply) {
    Str out(line);
    out += '\n';
    if (write(to, out.data(), out.size()) != (ssize_t)out.size()) return false;
    return ReadLine(reply);
  }

  void Close() {
    for (int fd : {to, from, mem})
      if (fd >= 0) close(fd);
    to = from = mem = -1;
  }
};

uint64_t GuardPages(int pagemap, uint64_t start, uint64_t end) {
  uint64_t count = 0;
  for (uint64_t at = start; at < end;) {
    uint64_t entries[512], pages = std::min<uint64_t>(512, (end - at) / kPage);
    if (pread(pagemap, entries, pages * 8, at / kPage * 8) != (ssize_t)(pages * 8)) break;
    for (uint64_t k = 0; k < pages; ++k) count += (entries[k] & kPagemapGuardRegion) != 0;
    at += pages * kPage;
  }
  return count;
}

struct FuzzMirror {
  using Region = MemMeld::Region;

  struct Rec {
    uintptr_t remote;
    Span<char> local;
    int prot;
    Str path;
    Size file_offset;
  };

  bool check_paths;
  std::mutex mutex;
  std::condition_variable cv;
  std::map<uintptr_t, Rec> model;
  Vec<std::pair<uintptr_t, uint64_t>> removed;
  Str event_error;
  bool attached = false;
  bool detached = false;
  Str detached_status;
  uintptr_t marker = 0;
  uint64_t marker_changes = 0;
  MemMeld mem_meld;
  std::thread thread;

  FuzzMirror(bool check_paths) : check_paths(check_paths) {
    mem_meld.on_attached = [this] { OnAttached(); };
    mem_meld.on_mapped = [this](const Region& r) { OnMapped(r); };
    mem_meld.on_changed = [this](const Region& before, const Region& after) {
      OnChanged(before, after);
    };
    mem_meld.on_unmapped = [this](const Region& r) { OnUnmapped(r); };
  }

  void Attach(int pidfd) {
    int own = fcntl(pidfd, F_DUPFD_CLOEXEC, 0);
    thread = std::thread([this, own] {
      SetThreadName("MemMeld");
      Status status;
      mem_meld.Attach(own, status);
      close(own);
      OnDetached(status);
    });
  }

  void Detach() {
    mem_meld.Detach();
    if (thread.joinable()) thread.join();
  }

  static Str Describe(uintptr_t remote, Span<char> local, int prot, StrView path, Size offset) {
    return f("[{:#x}, {:#x}) {} at local {} path '{}' file offset {:#x}", remote,
             remote + local.size(), ProtName(prot), (void*)local.data(), path, offset);
  }
  Str Describe(const Region& r) const {
    return Describe(r.remote, r.local, r.prot, check_paths ? r.path : StrView(), r.file_offset);
  }
  static Str Describe(const Rec& r) {
    return Describe(r.remote, r.local, r.prot, r.path, r.file_offset);
  }

  bool Same(const Rec& rec, const Region& r) const {
    return rec.remote == r.remote && rec.local.data() == r.local.data() &&
           rec.local.size() == r.local.size() && rec.prot == r.prot &&
           rec.file_offset == r.file_offset && (!check_paths || rec.path == r.path);
  }

  void Error(Str text) {
    if (event_error.empty()) event_error = std::move(text);
  }

  void Admit(const Region& r, StrView event) {
    Span<char> all = mem_meld.Local();
    uint64_t len = r.local.size();
    if (len == 0 || len % kPage || r.remote % kPage)
      Error(f("{} reported an unaligned or empty region {}", event, Describe(r)));
    if (r.local.data() < all.data() || r.local.data() + len > all.data() + all.size())
      Error(f("{} reported {} outside Local() [{}, +{:#x})", event, Describe(r), (void*)all.data(),
              all.size()));
    auto next = model.lower_bound(r.remote);
    if (next != model.end() && next->first < r.remote + len)
      Error(f("{} reported {} overlapping {}", event, Describe(r), Describe(next->second)));
    if (next != model.begin()) {
      auto& prev = std::prev(next)->second;
      if (prev.remote + prev.local.size() > r.remote)
        Error(f("{} reported {} overlapping {}", event, Describe(r), Describe(prev)));
    }
    for (auto& [remote, rec] : model)
      if (rec.local.data() < r.local.data() + len &&
          r.local.data() < rec.local.data() + rec.local.size())
        Error(f("{} reported {} whose local bytes overlap {}", event, Describe(r), Describe(rec)));
    const Region* found = mem_meld.Find(r.remote);
    if (!found || found->remote != r.remote || found->local.data() != r.local.data() ||
        found->local.size() != len || found->prot != r.prot)
      Error(f("during {} of {}, Find returned {}", event, Describe(r),
              found ? Describe(*found) : Str("null")));
    model[r.remote] =
        Rec{r.remote, r.local, r.prot, check_paths ? Str(r.path) : Str(), r.file_offset};
  }

  void Retire(const Region& r, StrView event) {
    auto it = model.find(r.remote);
    if (it == model.end()) {
      Error(f("{} of {} which was never reported", event, Describe(r)));
    } else {
      if (!Same(it->second, r))
        Error(
            f("{} of {} but the mirror had reported {}", event, Describe(r), Describe(it->second)));
      model.erase(it);
    }
    removed.push_back({r.remote, r.local.size()});
    if (removed.size() > 64) removed.erase(removed.begin());
  }

  void OnAttached() {
    std::lock_guard lock(mutex);
    attached = true;
    cv.notify_all();
  }
  void OnMapped(const Region& r) {
    std::lock_guard lock(mutex);
    Admit(r, "OnMapped");
    cv.notify_all();
  }
  void OnChanged(const Region& before, const Region& after) {
    std::lock_guard lock(mutex);
    Retire(before, "OnChanged");
    if (before.remote != after.remote && before.local.size() != after.local.size())
      Error(f("OnChanged moved and resized at once: {} -> {}", Describe(before), Describe(after)));
    Admit(after, "OnChanged");
    if (after.remote == marker) ++marker_changes;
    cv.notify_all();
  }
  void OnUnmapped(const Region& r) {
    std::lock_guard lock(mutex);
    Retire(r, "OnUnmapped");
    cv.notify_all();
  }
  void OnDetached(const Status& status) {
    std::lock_guard lock(mutex);
    detached = true;
    detached_status = status.ToStr();
    cv.notify_all();
  }
};

using Rec = FuzzMirror::Rec;

enum class Kind : uint8_t {
  kAttach,
  kMmap,
  kMmapFile,
  kMmapHuge,
  kMapMany,
  kMunmap,
  kMunmapWhole,
  kMremapShrink,
  kMremapWhole,
  kMremapMove,
  kMprotect,
  kMprotectHoles,
  kMadvise,
  kMadviseHoles,
  kWrite,
  kMirrorWrite,
  kVerify,
  kBrkGrow,
  kBrkShrink,
  kMalloc,
  kCalloc,
  kFree,
  kBlockWrite,
  kMallocTrim,
  kThreadsStart,
  kThreadsStop,
  kFork,
  kForkTest,
  kForkExec,
  kRecurse,
  kDlopen,
};
constexpr int kKinds = (int)Kind::kDlopen + 1;
constexpr const char* kKindNames[kKinds] = {
    "kAttach",      "kMmap",          "kMmapFile",     "kMmapHuge",     "kMapMany",
    "kMunmap",      "kMunmapWhole",   "kMremapShrink", "kMremapWhole",  "kMremapMove",
    "kMprotect",    "kMprotectHoles", "kMadvise",      "kMadviseHoles", "kWrite",
    "kMirrorWrite", "kVerify",        "kBrkGrow",      "kBrkShrink",    "kMalloc",
    "kCalloc",      "kFree",          "kBlockWrite",   "kMallocTrim",   "kThreadsStart",
    "kThreadsStop", "kFork",          "kForkTest",     "kForkExec",     "kRecurse",
    "kDlopen"};

struct Op {
  Kind kind;
  uint64_t a, b, c;
};

Str Describe(const Op& op) {
  return f("{{Kind::{}, {:#x}, {:#x}, {:#x}}}", kKindNames[(int)op.kind], op.a, op.b, op.c);
}

struct Profile {
  int before_attach[kKinds] = {};
  int after_attach[kKinds] = {};
  bool check_paths = false;
  bool check_leaks = false;
};

Profile FullProfile() {
  Profile p;
  auto before = [&](Kind kind, int weight) { p.before_attach[(int)kind] = weight; };
  auto after = [&](Kind kind, int weight) { p.after_attach[(int)kind] = weight; };
  before(Kind::kMmap, 30);
  before(Kind::kMmapFile, 10);
  before(Kind::kMapMany, 2);
  before(Kind::kMprotect, 10);
  before(Kind::kWrite, 20);
  before(Kind::kBrkGrow, 4);
  before(Kind::kMalloc, 6);
  before(Kind::kCalloc, 4);
  before(Kind::kThreadsStart, 3);
  after(Kind::kMmap, 120);
  after(Kind::kMmapFile, 20);
  after(Kind::kMmapHuge, 1);
  after(Kind::kMunmap, 60);
  after(Kind::kMunmapWhole, 40);
  after(Kind::kMremapShrink, 40);
  after(Kind::kMremapWhole, 60);
  after(Kind::kMremapMove, 60);
  after(Kind::kMprotect, 80);
  after(Kind::kMprotectHoles, 20);
  after(Kind::kMadvise, 50);
  after(Kind::kMadviseHoles, 10);
  after(Kind::kWrite, 120);
  after(Kind::kMirrorWrite, 80);
  after(Kind::kVerify, 40);
  after(Kind::kBrkGrow, 30);
  after(Kind::kBrkShrink, 30);
  after(Kind::kMalloc, 50);
  after(Kind::kCalloc, 30);
  after(Kind::kFree, 50);
  after(Kind::kBlockWrite, 40);
  after(Kind::kMallocTrim, 10);
  after(Kind::kThreadsStart, 10);
  after(Kind::kFork, 10);
  after(Kind::kForkTest, 10);
  after(Kind::kForkExec, 5);
  after(Kind::kRecurse, 20);
  after(Kind::kDlopen, 3);
  p.check_paths = Env("MEM_MELD_FUZZ_CHECK_PATHS", 0);
  p.check_leaks = Env("MEM_MELD_FUZZ_CHECK_LEAKS", 0);
  return p;
}

struct Failure {
  Str category;
  Str detail;
  explicit operator bool() const { return !category.empty(); }
};

struct Page {
  bool mapped = true;
  int prot = 0;
  bool defined = true;
  uint64_t file_offset = 0;
};

struct Extent {
  uint64_t offset, len, seed;
};

struct Area {
  uint64_t base[2];
  Vec<Page> pages;
  Vec<Extent> writes;
  bool file = false;
  bool brk = false;
  size_t step = 0;
};

struct Block {
  uint64_t addr[2];
  uint64_t len;
  bool zeroed;
  Vec<Extent> writes;
};

struct Interval {
  uint64_t start, end;
  const Vma* target;
  const Rec* region;
  const Vma* local;
};

struct Snapshot {
  Vec<Rec> regions;
  Vec<std::pair<uintptr_t, uint64_t>> removed;
  Vec<Vma> target;
  Vec<Vma> local;
  Span<char> mirror;
  Vec<Interval> intervals;
};

const Rec* RegionAt(const Vec<Rec>& regions, uint64_t addr) {
  auto it = std::upper_bound(regions.begin(), regions.end(), addr,
                             [](uint64_t a, const Rec& r) { return a < r.remote; });
  if (it == regions.begin()) return nullptr;
  --it;
  return addr < it->remote + it->local.size() ? &*it : nullptr;
}

bool Excluded(const Vma& v, uint64_t arg_start) {
  return v.path == "[vdso]" || v.path == "[vvar]" || v.path == "[vvar_vclock]" ||
         v.path == "[vsyscall]" || v.HasFlag("wf") || v.HasFlag("dc") || v.HasFlag("io") ||
         v.HasFlag("pf") || v.HasFlag("gd") || v.HasFlag("um") || v.HasFlag("uw") ||
         v.prot == PROT_NONE || (v.shared && v.path == "/dev/zero (deleted)") ||
         (!v.inode && v.start <= arg_start && arg_start < v.end);
}

uint64_t ArgStart(pid_t pid) {
  Str stat = ReadFile(f("/proc/{}/stat", pid));
  StrView rest = StrView(stat).substr(stat.rfind(')') + 2);
  for (int field = 3; field < 48 && !rest.empty(); ++field) rest.remove_prefix(rest.find(' ') + 1);
  return strtoull(Str(rest.substr(0, rest.find(' '))).c_str(), nullptr, 10);
}

uint64_t PagesFrom(uint64_t raw) {
  uint64_t bucket = raw % 100, rest = raw / 100;
  if (bucket < 70) return 1 + rest % 8;
  if (bucket < 90) return 9 + rest % 56;
  if (bucket < 99) return 65 + rest % 448;
  return 513 + rest % 3584;
}

uint64_t Addr(const Str& reply) { return strtoull(reply.c_str() + 3, nullptr, 16); }

void DropWrites(Vec<Extent>& writes, uint64_t lo, uint64_t hi) {
  std::erase_if(writes, [&](const Extent& e) { return e.offset < hi && lo < e.offset + e.len; });
}

struct Run {
  const Profile& profile;
  bool trace;
  Helper proc[2];
  std::unique_ptr<FuzzMirror> mirror;
  int memfd = -1;
  uint64_t memfd_inode = 0;
  uint64_t arg_start = 0;
  int marker_prot = PROT_READ | PROT_WRITE;
  Vec<Area> areas;
  Vec<Block> blocks;
  bool threads = false;
  size_t step = 0;
  Rng rng{1};
  double mirror_ratio = 0;

  Run(const Profile& profile, bool trace) : profile(profile), trace(trace) {}
  ~Run() { Stop(); }

  Failure Start() {
    for (int p : {kA, kB})
      if (Str error = proc[p].Spawn(Env("MEM_MELD_FUZZ_ASLR", 0)); !error.empty())
        return {"spawn", error};
    return {};
  }

  void Stop() {
    for (auto& helper : proc)
      if (helper.pid > 0) kill(helper.pid, SIGKILL);
    if (mirror) {
      {
        std::unique_lock lock(mirror->mutex);
        mirror->cv.wait_for(lock, 10s, [&] { return mirror->detached; });
      }
      mirror->Detach();
      mirror.reset();
    }
    for (auto& helper : proc) {
      if (helper.pid > 0) waitpid(helper.pid, nullptr, 0);
      helper.pid = 0;
      helper.Close();
    }
    if (memfd >= 0) close(memfd);
    memfd = -1;
  }

  bool IsMirrorMemfd(const Vma& v) const {
    return v.shared && v.inode == memfd_inode && v.path.starts_with("/memfd:pm-mirror");
  }

  Str DetachedStatus() {
    if (!mirror) return {};
    std::unique_lock lock(mirror->mutex);
    mirror->cv.wait_for(lock, 2s, [&] { return mirror->detached; });
    return mirror->detached ? mirror->detached_status : Str();
  }

  Failure Lost(int p, StrView command) {
    if (p == kB)
      return {"reference", f("the reference target stopped responding to '{}'", command)};
    Str status = DetachedStatus();
    return {proc[kA].hung ? "hang" : "died",
            f("the mirrored target {} after '{}'; mirror status: {}",
              proc[kA].hung ? "stopped responding" : "exited", command, status)};
  }

  Failure Send(const std::function<Str(int)>& line, Str (&reply)[2]) {
    for (int p : {kA, kB}) {
      Str text = line(p);
      if (!proc[p].Command(text, reply[p])) return Lost(p, text);
      if (trace) Print(f("    {} {} -> {}\n", p == kA ? 'A' : 'B', text, reply[p]));
    }
    return {};
  }

  Failure SendBoth(StrView what, const std::function<Str(int)>& line, Str (&reply)[2]) {
    if (Failure failure = Send(line, reply)) return failure;
    bool ok_a = reply[kA].starts_with("ok"), ok_b = reply[kB].starts_with("ok");
    if (ok_a != ok_b || (!ok_a && reply[kA] != reply[kB]))
      return {f("semantics({})", what),
              f("'{}' replied '{}' in the mirrored target and '{}' in the reference", line(kA),
                reply[kA], reply[kB])};
    return {};
  }

  int PickArea(uint64_t raw, const std::function<bool(const Area&)>& pred) {
    for (size_t k = 0; k < areas.size(); ++k) {
      size_t i = (raw + k) % areas.size();
      if (pred(areas[i])) return (int)i;
    }
    return -1;
  }

  static bool PickRun(const Area& area, uint64_t start_raw, uint64_t count_raw,
                      const std::function<bool(const Page& first, const Page&)>& pred,
                      size_t& first, size_t& count) {
    size_t n = area.pages.size();
    for (size_t k = 0; k < n; ++k) {
      size_t i = (start_raw + k) % n;
      if (!pred(area.pages[i], area.pages[i])) continue;
      size_t end = i + 1;
      while (end < n && pred(area.pages[i], area.pages[end])) ++end;
      first = i;
      count = 1 + count_raw % (end - i);
      return true;
    }
    return false;
  }

  static bool Mapped(const Page&, const Page& p) { return p.mapped; }
  static bool SameProt(const Page& first, const Page& p) {
    return p.mapped && first.mapped && p.prot == first.prot;
  }
  static bool Writable(const Page&, const Page& p) { return p.mapped && (p.prot & PROT_WRITE); }
  static bool AnyMapped(const Area& area) {
    return std::any_of(area.pages.begin(), area.pages.end(),
                       [](const Page& p) { return p.mapped; });
  }

  bool HolesFree(const Area& area, size_t first, size_t count) {
    for (int p : {kA, kB}) {
      Vec<Vma> vmas = ReadVmas(f("/proc/{}/maps", proc[p].pid));
      for (size_t i = first; i < first + count; ++i) {
        if (area.pages[i].mapped) continue;
        uint64_t lo = area.base[p] + i * kPage;
        for (const Vma& v : vmas)
          if (v.start < lo + kPage && lo < v.end) return false;
      }
    }
    return true;
  }

  void AddWrite(Vec<Extent>& writes, Vec<Page>* pages, uint64_t offset, uint64_t len,
                uint64_t seed) {
    DropWrites(writes, offset, offset + len);
    writes.push_back({offset, len, seed});
    if (pages)
      for (uint64_t page = (offset + kPage - 1) / kPage; (page + 1) * kPage <= offset + len; ++page)
        (*pages)[page].defined = true;
  }

  void Unmap(Area& area, size_t first, size_t count) {
    for (size_t i = first; i < first + count; ++i) area.pages[i].mapped = false;
    DropWrites(area.writes, first * kPage, (first + count) * kPage);
  }

  void Resize(Area& area, size_t pages) {
    if (pages < area.pages.size()) {
      area.pages.resize(pages);
      DropWrites(area.writes, pages * kPage, UINT64_MAX);
    } else {
      Page tail = area.pages.back();
      tail.defined = true;
      while (area.pages.size() < pages) {
        tail.file_offset += kPage;
        area.pages.push_back(tail);
      }
    }
  }

  Failure Execute(const Op& op) {
    if (trace) Print(f("  step {}: {}\n", step, Describe(op)));
    Failure failure = Dispatch(op);
    ++step;
    return failure;
  }

  Failure Dispatch(const Op& op) {
    switch (op.kind) {
      case Kind::kAttach:
        return Attach(op);
      case Kind::kMmap:
        return Mmap(op);
      case Kind::kMmapFile:
        return MmapFile(op);
      case Kind::kMmapHuge:
        return MmapHuge(op);
      case Kind::kMapMany:
        return MapMany(op);
      case Kind::kMunmap:
      case Kind::kMunmapWhole:
        return Munmap(op);
      case Kind::kMremapShrink:
        return MremapShrink(op);
      case Kind::kMremapWhole:
        return MremapWhole(op);
      case Kind::kMremapMove:
        return MremapMove(op);
      case Kind::kMprotect:
      case Kind::kMprotectHoles:
        return Mprotect(op);
      case Kind::kMadvise:
      case Kind::kMadviseHoles:
        return Madvise(op);
      case Kind::kWrite:
      case Kind::kMirrorWrite:
        return Write(op);
      case Kind::kVerify:
        return Verify(op.a);
      case Kind::kBrkGrow:
        return BrkGrow(op);
      case Kind::kBrkShrink:
        return BrkShrink(op);
      case Kind::kMalloc:
      case Kind::kCalloc:
        return Malloc(op);
      case Kind::kFree:
        return Free(op);
      case Kind::kBlockWrite:
        return BlockWrite(op);
      case Kind::kMallocTrim:
        return Simple("malloc_trim", "malloc_trim");
      case Kind::kThreadsStart:
        if (threads) return {};
        threads = true;
        return Simple("threads", f("threads {}", 1 + op.a % 4));
      case Kind::kThreadsStop:
        if (!threads) return {};
        threads = false;
        return Simple("threads_stop", "threads_stop");
      case Kind::kFork:
        return Simple("fork", "fork");
      case Kind::kForkTest:
        return ForkTest();
      case Kind::kForkExec:
        return Simple("forkexec", "forkexec");
      case Kind::kRecurse:
        return Simple("recurse", f("recurse {}", 1 + op.a % 3000));
      case Kind::kDlopen:
        return Simple("dlopen",
                      f("dlopen {}", kLibraries[op.a % (sizeof kLibraries / sizeof *kLibraries)]));
    }
    return {};
  }

  Failure Simple(StrView what, const Str& line) {
    Str reply[2];
    if (Failure failure = SendBoth(what, [&](int) { return line; }, reply)) return failure;
    if (!line.starts_with("dlopen") && reply[kA] != reply[kB])
      return {f("semantics({})", what), f("'{}' replied '{}' in the mirrored target and '{}' in "
                                          "the reference",
                                          line, reply[kA], reply[kB])};
    return {};
  }

  Failure ForkTest() {
    Str reply[2];
    if (Failure failure = SendBoth("forktest", [](int) { return Str("forktest"); }, reply))
      return failure;
    for (int p : {kA, kB}) {
      size_t parent = reply[p].find("parent="), child = reply[p].find("child=");
      if (!reply[p].starts_with("ok mutex=ok") || parent == Str::npos || child == Str::npos ||
          reply[p].substr(parent + 7, 32) == reply[p].substr(child + 6, 32))
        return {"semantics(forktest)", f("forktest replied '{}' in the {} target", reply[p],
                                         p == kA ? "mirrored" : "reference")};
    }
    return {};
  }

  Failure Attach(const Op& op) {
    if (mirror) return {};
    constexpr rlim_t kStackLimits[] = {0, 16 << 20, RLIM_INFINITY};
    if (rlim_t limit = kStackLimits[op.a % 3]) {
      for (int p : {kA, kB}) {
        rlimit current;
        prlimit(proc[p].pid, RLIMIT_STACK, nullptr, &current);
        current.rlim_cur = limit;
        current.rlim_max = RLIM_INFINITY;
        prlimit(proc[p].pid, RLIMIT_STACK, &current, nullptr);
      }
    }
    int pidfd = (int)syscall(SYS_pidfd_open, proc[kA].pid, 0);
    if (pidfd < 0) return {"attach", "pidfd_open"};
    mirror = std::make_unique<FuzzMirror>(profile.check_paths);
    mirror->Attach(pidfd);
    close(pidfd);
    {
      std::unique_lock lock(mirror->mutex);
      if (!mirror->cv.wait_for(lock, 20s, [&] { return mirror->attached || mirror->detached; }))
        return {"attach", "neither OnAttached nor OnDetached arrived"};
      if (!mirror->attached) return {"attach", mirror->detached_status};
    }
    Str reply;
    if (!proc[kA].Command("mmap 4096 rw", reply)) return Lost(kA, "mmap 4096 rw");
    {
      std::lock_guard lock(mirror->mutex);
      mirror->marker = Addr(reply);
    }
    if (Failure failure = Barrier()) return failure;
    Str dir = f("/proc/{}/fd", proc[kA].pid);
    if (DIR* d = opendir(dir.c_str())) {
      while (dirent* e = readdir(d)) {
        char link[256] = {};
        Str path = dir + "/" + e->d_name;
        if (readlink(path.c_str(), link, sizeof link - 1) > 0 &&
            StrView(link).starts_with("/memfd:pm-mirror"))
          memfd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
      }
      closedir(d);
    }
    struct stat st;
    if (memfd < 0 || fstat(memfd, &st))
      return {"attach", "the mirror memfd is not open in the target"};
    memfd_inode = st.st_ino;
    arg_start = ArgStart(proc[kA].pid);
    return {};
  }

  Failure Barrier() {
    uint64_t want;
    uintptr_t marker;
    {
      std::lock_guard lock(mirror->mutex);
      want = mirror->marker_changes + 1;
      marker = mirror->marker;
    }
    marker_prot = marker_prot == PROT_READ ? PROT_READ | PROT_WRITE : PROT_READ;
    Str line = f("mprotect {:#x} 4096 {}", marker, ProtName(marker_prot)), reply;
    if (!proc[kA].Command(line, reply)) return Lost(kA, line);
    if (reply != "ok") return {"barrier", f("'{}' replied '{}'", line, reply)};
    std::unique_lock lock(mirror->mutex);
    if (!mirror->cv.wait_for(lock, 20s,
                             [&] { return mirror->marker_changes >= want || mirror->detached; }))
      return {"barrier", "the change of the marker page did not arrive"};
    if (mirror->detached) return {"detached", mirror->detached_status};
    return {};
  }

  Failure Mmap(const Op& op) {
    uint64_t pages = PagesFrom(op.a);
    int prot = kProts[op.b % 5];
    uint64_t len = pages * kPage - (op.c % 4 == 0 ? op.c / 4 % kPage : 0);
    Str reply[2];
    if (Failure failure =
            SendBoth("mmap", [&](int) { return f("mmap {} {}", len, ProtName(prot)); }, reply))
      return failure;
    if (!reply[kA].starts_with("ok")) return {};
    Area area{.base = {Addr(reply[kA]), Addr(reply[kB])}, .step = step};
    area.pages.assign(pages, Page{.prot = prot});
    areas.push_back(std::move(area));
    return {};
  }

  Failure MmapFile(const Op& op) {
    uint64_t pages = 1 + op.a % kFilePages;
    int prot = kFileProts[op.b % 3];
    Str reply[2];
    if (Failure failure = SendBoth(
            "mmapfile",
            [&](int) { return f("mmapfile {} {} {}", File().path, pages * kPage, ProtName(prot)); },
            reply))
      return failure;
    if (!reply[kA].starts_with("ok")) return {};
    Area area{.base = {Addr(reply[kA]), Addr(reply[kB])}, .file = true, .step = step};
    for (uint64_t i = 0; i < pages; ++i)
      area.pages.push_back({.prot = prot, .file_offset = i * kPage});
    areas.push_back(std::move(area));
    return {};
  }

  Failure MmapHuge(const Op& op) {
    uint64_t len = (64ull << 30) + (1 + op.a % 16) * kPage;
    Str reply[2];
    return Send([&](int p) { return f("mmap {} {}", len, p == kA ? "rw" : "none"); }, reply);
  }

  Failure MapMany(const Op& op) {
    uint64_t count = 200 + op.a % 2800;
    for (uint64_t i = 0; i < count; ++i) {
      Str reply[2];
      if (Failure failure =
              SendBoth("mmap", [&](int) { return f("mmap 4096 {}", i % 2 ? "rw" : "r"); }, reply))
        return failure;
    }
    return {};
  }

  Failure Munmap(const Op& op) {
    int index = PickArea(op.a, [](const Area& area) { return !area.brk && AnyMapped(area); });
    if (index < 0) return {};
    Area& area = areas[index];
    size_t n = area.pages.size(), first = 0, count = n;
    if (op.kind == Kind::kMunmap) {
      first = op.b % n;
      count = 1 + op.c % (n - first);
    }
    if (!HolesFree(area, first, count)) return {};
    Str reply[2];
    if (Failure failure = SendBoth(
            "munmap",
            [&](int p) {
              return f("munmap {:#x} {}", area.base[p] + first * kPage, count * kPage);
            },
            reply))
      return failure;
    if (!reply[kA].starts_with("ok")) return {};
    Unmap(area, first, count);
    if (!AnyMapped(area)) areas.erase(areas.begin() + index);
    return {};
  }

  Failure MremapShrink(const Op& op) {
    int index = PickArea(op.a, [](const Area& area) { return !area.brk && AnyMapped(area); });
    if (index < 0) return {};
    Area& area = areas[index];
    size_t first, count;
    if (!PickRun(area, op.b, op.c, SameProt, first, count) || count < 2) return {};
    size_t keep = 1 + op.a / 7 % (count - 1);
    Str reply[2];
    if (Failure failure = SendBoth(
            "mremap",
            [&](int p) {
              return f("mremap {:#x} {} {}", area.base[p] + first * kPage, count * kPage,
                       keep * kPage);
            },
            reply))
      return failure;
    if (!reply[kA].starts_with("ok")) return {};
    for (int p : {kA, kB})
      if (Addr(reply[p]) != area.base[p] + first * kPage)
        return {"semantics(mremap)", f("a shrinking mremap moved the pages in the {} target",
                                       p == kA ? "mirrored" : "reference")};
    Unmap(area, first + keep, count - keep);
    return {};
  }

  Failure MremapWhole(const Op& op) {
    int index = PickArea(op.a, [](const Area& area) {
      return !area.brk && std::all_of(area.pages.begin(), area.pages.end(),
                                      [&](const Page& p) { return SameProt(area.pages[0], p); });
    });
    if (index < 0) return {};
    Area& area = areas[index];
    size_t n = area.pages.size();
    size_t pages = area.file ? 1 + op.c % n : 1 + op.c % (2 * n + 8);
    Str reply[2];
    if (Failure failure = SendBoth(
            "mremap",
            [&](int p) { return f("mremap {:#x} {} {}", area.base[p], n * kPage, pages * kPage); },
            reply))
      return failure;
    if (!reply[kA].starts_with("ok")) return {};
    for (int p : {kA, kB}) area.base[p] = Addr(reply[p]);
    Resize(area, pages);
    return {};
  }

  Failure MremapMove(const Op& op) {
    int index = PickArea(op.a, [](const Area& area) { return !area.brk && AnyMapped(area); });
    if (index < 0) return {};
    size_t first, count;
    if (!PickRun(areas[index], op.b, op.c, SameProt, first, count)) return {};
    Area& area = areas[index];
    size_t pages = area.file ? 1 + op.a / 7 % count : 1 + op.a / 7 % (2 * count + 4);
    Str reply[2];
    if (Failure failure = SendBoth(
            "mremapmove",
            [&](int p) {
              return f("mremapmove {:#x} {} {}", area.base[p] + first * kPage, count * kPage,
                       pages * kPage);
            },
            reply))
      return failure;
    if (!reply[kA].starts_with("ok")) return {};
    if (first == 0 && count == area.pages.size()) {
      for (int p : {kA, kB}) area.base[p] = Addr(reply[p]);
      Resize(area, pages);
      return {};
    }
    Area moved{.base = {Addr(reply[kA]), Addr(reply[kB])}, .file = area.file, .step = step};
    moved.pages.assign(area.pages.begin() + first, area.pages.begin() + first + count);
    for (const Extent& e : area.writes)
      if (e.offset >= first * kPage && e.offset + e.len <= (first + count) * kPage)
        moved.writes.push_back({e.offset - first * kPage, e.len, e.seed});
    Resize(moved, pages);
    Unmap(area, first, count);
    if (!AnyMapped(area)) areas.erase(areas.begin() + index);
    areas.push_back(std::move(moved));
    return {};
  }

  Failure Mprotect(const Op& op) {
    bool holes = op.kind == Kind::kMprotectHoles;
    int index = PickArea(op.a, [](const Area& area) { return AnyMapped(area); });
    if (index < 0) return {};
    Area& area = areas[index];
    size_t first, count, n = area.pages.size();
    if (holes) {
      first = op.b % n;
      count = 1 + op.c % (n - first);
      if (!HolesFree(area, first, count)) return {};
    } else if (!PickRun(area, op.b, op.c, Mapped, first, count)) {
      return {};
    }
    int prot = kProts[op.a / 7 % 5];
    Str reply[2];
    if (Failure failure = SendBoth(
            "mprotect",
            [&](int p) {
              return f("mprotect {:#x} {} {}", area.base[p] + first * kPage, count * kPage,
                       ProtName(prot));
            },
            reply))
      return failure;
    for (size_t i = first; i < first + count && area.pages[i].mapped; ++i)
      area.pages[i].prot = prot;
    return {};
  }

  Failure Madvise(const Op& op) {
    bool holes = op.kind == Kind::kMadviseHoles;
    int index = PickArea(op.a, [](const Area& area) { return AnyMapped(area); });
    if (index < 0) return {};
    Area& area = areas[index];
    size_t first, count, n = area.pages.size();
    if (holes) {
      first = op.b % n;
      count = 1 + op.c % (n - first);
      if (!HolesFree(area, first, count)) return {};
    } else if (!PickRun(area, op.b, op.c, Mapped, first, count)) {
      return {};
    }
    bool dontneed = op.a / 7 % 2;
    Str reply[2];
    if (Failure failure = SendBoth(
            "madvise",
            [&](int p) {
              return f("madvise {:#x} {} {}", area.base[p] + first * kPage, count * kPage,
                       dontneed ? "dontneed" : "free");
            },
            reply))
      return failure;
    if (!reply[kA].starts_with("ok") && !reply[kA].starts_with("err ENOMEM")) return {};
    for (size_t i = first; i < first + count; ++i)
      if (area.pages[i].mapped) area.pages[i].defined = dontneed && !area.file;
    DropWrites(area.writes, first * kPage, (first + count) * kPage);
    return {};
  }

  Failure Write(const Op& op) {
    int index = PickArea(op.a, [](const Area& area) {
      return std::any_of(area.pages.begin(), area.pages.end(),
                         [](const Page& p) { return Writable(p, p); });
    });
    if (index < 0) return {};
    Area& area = areas[index];
    size_t first, count;
    if (!PickRun(area, op.b, op.c, Writable, first, count)) return {};
    uint64_t start = first * kPage + op.c / 7 % (count * kPage);
    uint64_t len = 1 + op.a / 7 % std::min<uint64_t>((first + count) * kPage - start, 256 << 10);
    uint64_t seed = op.a ^ op.b;
    if (op.kind == Kind::kMirrorWrite) {
      if (Failure failure = MirrorWrite(area.base[kA] + start, area.base[kB] + start, len, seed))
        return failure.category == "skip" ? Failure{} : failure;
    } else {
      Str reply[2];
      if (Failure failure = SendBoth(
              "write",
              [&](int p) { return f("write {:#x} {} {}", area.base[p] + start, len, seed); },
              reply))
        return failure;
    }
    AddWrite(area.writes, &area.pages, start, len, seed);
    return {};
  }

  Failure MirrorWrite(uint64_t remote, uint64_t reference, uint64_t len, uint64_t seed) {
    if (!mirror) return {"skip", {}};
    Vec<Span<char>> spans = [&] {
      Vec<Span<char>> out;
      for (uint64_t done = 0; done < len;) {
        const MemMeld::Region* region = mirror->mem_meld.Find(remote + done);
        if (!region || !(region->prot & PROT_WRITE)) return Vec<Span<char>>();
        uint64_t n =
            std::min<uint64_t>(region->remote + region->local.size() - remote - done, len - done);
        Span<char> local = mirror->mem_meld.ToLocal(remote + done, n);
        if (local.empty()) return Vec<Span<char>>();
        out.push_back(local);
        done += n;
      }
      return out;
    }();
    if (spans.empty()) return {"skip", {}};
    Vec<Vma> local_maps = ReadVmas("/proc/self/maps");
    for (Span<char> span : spans)
      for (uint64_t at = (uint64_t)span.data(); at < (uint64_t)span.data() + span.size();) {
        const Vma* v = Covering(local_maps, at);
        if (!v || !(v->prot & PROT_WRITE))
          return {"backing", f("the mirror reports the target's pages at {:#x} writable but their "
                               "local bytes at {:#x} are not",
                               remote, at)};
        at = v->end;
      }
    Vec<char> bytes(len);
    for (uint64_t i = 0; i < len; ++i) bytes[i] = (char)PatternByte(seed, i);
    uint64_t done = 0;
    for (Span<char> span : spans) {
      memcpy(span.data(), bytes.data() + done, span.size());
      done += span.size();
    }
    iovec local{bytes.data(), len}, target{(void*)reference, len};
    if (process_vm_writev(proc[kB].pid, &local, 1, &target, 1, 0) != (ssize_t)len)
      return {"reference", "process_vm_writev into the reference target failed"};
    if (trace) Print(f("    mirror write {:#x} {} {}\n", remote, len, seed));
    return {};
  }

  Failure Verify(uint64_t raw) {
    struct Candidate {
      uint64_t addr[2];
      Extent extent;
    };
    Vec<Candidate> candidates;
    for (const Area& area : areas)
      for (const Extent& e : area.writes) {
        bool readable = true;
        for (uint64_t page = e.offset / kPage; page * kPage < e.offset + e.len; ++page)
          readable &= area.pages[page].mapped && (area.pages[page].prot & PROT_READ);
        if (readable) candidates.push_back({{area.base[kA], area.base[kB]}, e});
      }
    for (const Block& block : blocks)
      for (const Extent& e : block.writes)
        candidates.push_back({{block.addr[kA], block.addr[kB]}, e});
    if (candidates.empty()) return {};
    const Candidate& c = candidates[raw % candidates.size()];
    Str reply[2];
    if (Failure failure = Send(
            [&](int p) {
              return f("verify {:#x} {} {}", c.addr[p] + c.extent.offset, c.extent.len,
                       c.extent.seed);
            },
            reply))
      return failure;
    if (reply[kB] != "ok")
      return {"reference", f("the reference target replied '{}' to verify", reply[kB])};
    if (reply[kA] != "ok")
      return {"verify", f("verify {:#x} {} {} replied '{}' in the mirrored target",
                          c.addr[kA] + c.extent.offset, c.extent.len, c.extent.seed, reply[kA])};
    return {};
  }

  Area* TopBrkSegment() {
    for (auto it = areas.rbegin(); it != areas.rend(); ++it)
      if (it->brk) return &*it;
    return nullptr;
  }

  Failure BrkGrow(const Op& op) {
    uint64_t pages = 1 + op.a % 32;
    Str reply[2];
    if (Failure failure = SendBoth("brk", [&](int) { return f("brk {}", pages * kPage); }, reply))
      return failure;
    if (!reply[kA].starts_with("ok")) return {};
    Area area{.base = {Addr(reply[kA]) - pages * kPage, Addr(reply[kB]) - pages * kPage},
              .brk = true,
              .step = step};
    if (area.base[kA] % kPage || area.base[kB] % kPage) return {};
    area.pages.assign(pages, Page{.prot = PROT_READ | PROT_WRITE});
    areas.push_back(std::move(area));
    return {};
  }

  Failure BrkShrink(const Op& op) {
    Area* segment = TopBrkSegment();
    if (!segment) return {};
    Str reply[2];
    if (Failure failure = SendBoth("brk", [](int) { return Str("brk 0"); }, reply)) return failure;
    for (int p : {kA, kB})
      if (Addr(reply[p]) != segment->base[p] + segment->pages.size() * kPage) return {};
    size_t pages = 1 + op.a % segment->pages.size();
    if (Failure failure = SendBoth("brk", [&](int) { return f("brk -{}", pages * kPage); }, reply))
      return failure;
    if (!reply[kA].starts_with("ok")) return {};
    if (pages == segment->pages.size())
      areas.erase(areas.begin() + (segment - areas.data()));
    else
      Resize(*segment, segment->pages.size() - pages);
    return {};
  }

  Failure Malloc(const Op& op) {
    bool zeroed = op.kind == Kind::kCalloc;
    uint64_t len, count = 1 + op.a % 64, size = 1 + op.b % 8192;
    if (zeroed)
      len = count * size;
    else if (op.a % 10 < 5)
      len = 1 + op.b % 512;
    else if (op.a % 10 < 9)
      len = 513 + op.b % 65536;
    else
      len = (128 << 10) + op.b % (1 << 20);
    Str reply[2];
    if (Failure failure = SendBoth(
            zeroed ? "calloc" : "malloc",
            [&](int) { return zeroed ? f("calloc {} {}", count, size) : f("malloc {}", len); },
            reply))
      return failure;
    if (!reply[kA].starts_with("ok")) return {};
    blocks.push_back({{Addr(reply[kA]), Addr(reply[kB])}, len, zeroed, {}});
    return {};
  }

  Failure Free(const Op& op) {
    if (blocks.empty()) return {};
    size_t index = op.a % blocks.size();
    Str reply[2];
    if (Failure failure =
            SendBoth("free", [&](int p) { return f("free {:#x}", blocks[index].addr[p]); }, reply))
      return failure;
    blocks.erase(blocks.begin() + index);
    return {};
  }

  Failure BlockWrite(const Op& op) {
    if (blocks.empty()) return {};
    Block& block = blocks[op.a % blocks.size()];
    uint64_t start = op.b % block.len;
    uint64_t len = 1 + op.c % std::min<uint64_t>(block.len - start, 256 << 10);
    uint64_t seed = op.b ^ op.c;
    Str reply[2];
    if (Failure failure = SendBoth(
            "write",
            [&](int p) { return f("write {:#x} {} {}", block.addr[p] + start, len, seed); }, reply))
      return failure;
    AddWrite(block.writes, nullptr, start, len, seed);
    return {};
  }

  bool ReadA(const Snapshot& s, uint64_t addr, char* buf, uint64_t len) {
    for (uint64_t at = addr; at < addr + len;) {
      const Vma* v = Covering(s.target, at);
      if (!v) return false;
      uint64_t end = std::min(v->end, addr + len);
      char* out = buf + (at - addr);
      if (IsMirrorMemfd(*v)) {
        memset(out, 0, end - at);
        uint64_t offset = v->offset + (at - v->start);
        bool ok = true;
        ForEachData(memfd, offset, offset + (end - at), [&](uint64_t data, uint64_t n) {
          ok &= proc[kA].Read(at + (data - offset), out + (data - offset), n);
        });
        if (!ok) return false;
      } else if (!proc[kA].Read(at, out, end - at)) {
        return false;
      }
      at = end;
    }
    return true;
  }

  Failure Check() {
    if (!mirror || threads) return {};
    if (Failure failure = Barrier()) return failure;
    Snapshot s;
    {
      std::lock_guard lock(mirror->mutex);
      if (!mirror->event_error.empty()) return {"events", mirror->event_error};
      if (mirror->detached) return {"detached", mirror->detached_status};
      for (auto& [remote, rec] : mirror->model) s.regions.push_back(rec);
      s.removed = mirror->removed;
    }
    s.target = ReadVmas(f("/proc/{}/smaps", proc[kA].pid));
    s.mirror = mirror->mem_meld.Local();
    s.local = ReadVmas("/proc/self/maps");
    BuildIntervals(s);
    for (auto check :
         {&Run::CheckRegions, &Run::CheckCoverage, &Run::CheckAliasing, &Run::CheckLocalMirror,
          &Run::CheckTranslation, &Run::CheckContent, &Run::CheckReference, &Run::CheckLeaks})
      if (Failure failure = (this->*check)(s)) return failure;
    for (uint64_t i = 0; i < 2; ++i)
      if (Failure failure = Verify(rng.Next())) return failure;
    return {};
  }

  void BuildIntervals(Snapshot& s) {
    Vec<uint64_t> cuts;
    for (const Vma& v : s.target) cuts.insert(cuts.end(), {v.start, v.end});
    for (const Rec& r : s.regions) {
      uint64_t lo = (uint64_t)r.local.data(), hi = lo + r.local.size();
      cuts.insert(cuts.end(), {r.remote, r.remote + r.local.size()});
      auto it = std::upper_bound(s.local.begin(), s.local.end(), lo,
                                 [](uint64_t a, const Vma& v) { return a < v.end; });
      for (; it != s.local.end() && it->start < hi; ++it) {
        if (it->start > lo) cuts.push_back(r.remote + (it->start - lo));
        if (it->end < hi) cuts.push_back(r.remote + (it->end - lo));
      }
    }
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    for (size_t i = 0; i + 1 < cuts.size(); ++i) {
      const Vma* target = Covering(s.target, cuts[i]);
      const Rec* region = RegionAt(s.regions, cuts[i]);
      if (!target && !region) continue;
      const Vma* local =
          region ? Covering(s.local, (uint64_t)region->local.data() + (cuts[i] - region->remote))
                 : nullptr;
      s.intervals.push_back({cuts[i], cuts[i + 1], target, region, local});
    }
  }

  Failure CheckRegions(const Snapshot& s) {
    for (size_t i = 0; i + 1 < s.regions.size(); ++i)
      if (s.regions[i].remote + s.regions[i].local.size() > s.regions[i + 1].remote)
        return {"overlap", f("{} overlaps {}", FuzzMirror::Describe(s.regions[i]),
                             FuzzMirror::Describe(s.regions[i + 1]))};
    Vec<const Rec*> by_local;
    for (const Rec& r : s.regions) by_local.push_back(&r);
    std::sort(by_local.begin(), by_local.end(),
              [](const Rec* a, const Rec* b) { return a->local.data() < b->local.data(); });
    for (size_t i = 0; i + 1 < by_local.size(); ++i)
      if (by_local[i]->local.data() + by_local[i]->local.size() > by_local[i + 1]->local.data())
        return {"overlap", f("the local bytes of {} overlap {}", FuzzMirror::Describe(*by_local[i]),
                             FuzzMirror::Describe(*by_local[i + 1]))};
    for (const Rec& r : s.regions)
      if (r.local.data() < s.mirror.data() ||
          r.local.data() + r.local.size() > s.mirror.data() + s.mirror.size())
        return {"mirror", f("{} lies outside Local() [{}, +{:#x})", FuzzMirror::Describe(r),
                            (void*)s.mirror.data(), s.mirror.size())};
    return {};
  }

  Failure CheckCoverage(const Snapshot& s) {
    FD pagemap = open(f("/proc/{}/pagemap", proc[kA].pid).c_str(), O_RDONLY | O_CLOEXEC);
    for (const Interval& in : s.intervals) {
      Str where = f("[{:#x}, {:#x})", in.start, in.end);
      if (!in.target)
        return {"stale-region", f("the mirror holds {} but the target maps nothing at {}",
                                  FuzzMirror::Describe(*in.region), where)};
      const Vma& t = *in.target;
      bool converted = IsMirrorMemfd(t);
      if (converted ? t.prot == PROT_NONE : t.shared || Excluded(t, arg_start)) {
        if (in.region)
          return {"excluded-mirrored",
                  f("the mirror holds {} over {} of {}, which the design leaves unmirrored",
                    FuzzMirror::Describe(*in.region), where, DescribeVma(t))};
        continue;
      }
      if (!in.region) {
        if (GuardPages(pagemap, in.start, in.end) == (in.end - in.start) / kPage) continue;
        return {f("unmirrored({})", converted ? "converted"
                                    : t.inode ? "file"
                                              : "anonymous"),
                f("target pages {} of {} are not mirrored", where, DescribeVma(t))};
      }
      const Rec& r = *in.region;
      if (r.prot != t.prot)
        return {"prot", f("the mirror holds {} with protection {} but the target has {} at {}",
                          FuzzMirror::Describe(r), ProtName(r.prot), DescribeVma(t), where)};
      if (!in.local)
        return {"backing",
                f("the local bytes of {} at {} are not mapped", FuzzMirror::Describe(r), where)};
      uint64_t local_offset =
          in.local->offset + ((uint64_t)r.local.data() + (in.start - r.remote) - in.local->start);
      uint64_t target_offset = t.offset + (in.start - t.start);
      if (converted) {
        if (!IsMirrorMemfd(*in.local) || local_offset != target_offset)
          return {"backing", f("{} maps {} at offset {:#x} locally at {} where the target maps "
                               "memfd offset {:#x}",
                               FuzzMirror::Describe(r), DescribeVma(*in.local), local_offset, where,
                               target_offset)};
      } else if (!t.inode || (t.prot & PROT_WRITE)) {
        return {"unconverted", f("the target's private pages {} of {} are not memfd pages, so the "
                                 "mirror does not share them",
                                 where, DescribeVma(t))};
      } else if (in.local->inode != t.inode || (in.local->prot & PROT_WRITE) ||
                 local_offset != target_offset) {
        return {"backing",
                f("{} shows {} at {} instead of a read-only mapping of the target's {}",
                  FuzzMirror::Describe(r), DescribeVma(*in.local), where, DescribeVma(t))};
      } else if (profile.check_paths &&
                 (r.path != t.path || r.file_offset + (in.start - r.remote) != target_offset)) {
        return {"path", f("{} at {} names the file differently from the target's {}",
                          FuzzMirror::Describe(r), where, DescribeVma(t))};
      }
    }
    if (profile.check_paths)
      for (const Area& area : areas)
        for (size_t i = 0; area.file && i < area.pages.size(); ++i) {
          const Rec* r =
              area.pages[i].mapped ? RegionAt(s.regions, area.base[kA] + i * kPage) : nullptr;
          if (r && (r->path != File().maps_path ||
                    r->file_offset + (area.base[kA] + i * kPage - r->remote) !=
                        area.pages[i].file_offset))
            return {"path",
                    f("{} holds page {} of a mapping of '{}' at file offset {:#x}",
                      FuzzMirror::Describe(*r), i, File().maps_path, area.pages[i].file_offset)};
        }
    return {};
  }

  Failure CheckAliasing(const Snapshot& s) {
    struct Use {
      uint64_t lo, hi;
      Str who;
    };
    Vec<Use> uses;
    for (const Vma& t : s.target)
      if (IsMirrorMemfd(t))
        uses.push_back({t.offset, t.offset + (t.end - t.start),
                        f("target mapping [{:#x}, {:#x})", t.start, t.end)});
    for (const Interval& in : s.intervals) {
      if (!in.region || !in.local || !IsMirrorMemfd(*in.local)) continue;
      if (in.target && IsMirrorMemfd(*in.target)) continue;
      uint64_t lo = in.local->offset + ((uint64_t)in.region->local.data() +
                                        (in.start - in.region->remote) - in.local->start);
      uses.push_back(
          {lo, lo + (in.end - in.start), f("mirror copy of [{:#x}, {:#x})", in.start, in.end)});
    }
    std::sort(uses.begin(), uses.end(), [](const Use& a, const Use& b) { return a.lo < b.lo; });
    for (size_t i = 1; i < uses.size(); ++i)
      for (size_t j = i; j-- > 0;) {
        if (uses[j].hi <= uses[i].lo) continue;
        return {"aliasing", f("{} and {} share memfd offsets [{:#x}, {:#x})", uses[j].who,
                              uses[i].who, uses[i].lo, std::min(uses[i].hi, uses[j].hi))};
      }
    return {};
  }

  Failure CheckLocalMirror(const Snapshot& s) {
    Vec<std::pair<uint64_t, uint64_t>> held;
    for (const Rec& r : s.regions)
      held.push_back({(uint64_t)r.local.data(), (uint64_t)r.local.data() + r.local.size()});
    std::sort(held.begin(), held.end());
    uint64_t lo = (uint64_t)s.mirror.data(), hi = lo + s.mirror.size(), at = lo;
    for (const Vma& v : s.local) {
      if (v.end <= lo || v.start >= hi) continue;
      if (v.start > at)
        return {"mirror", f("Local() has no mapping at [{:#x}, {:#x})", at, v.start)};
      if (!(v.prot & PROT_READ))
        return {"mirror", f("Local() has unreadable pages {}", DescribeVma(v))};
      if (IsMirrorMemfd(v)) {
        uint64_t page = std::max(v.start, lo), end = std::min(v.end, hi);
        auto it = std::upper_bound(held.begin(), held.end(), std::pair(page, UINT64_MAX));
        if (it != held.begin() && std::prev(it)->second > page) --it;
        for (; page < end && it != held.end() && it->first <= page; ++it)
          page = std::max(page, it->second);
        if (page < end)
          return {"mirror", f("the page {:#x} of Local() belongs to no region but still maps "
                              "memfd offset {:#x}",
                              page, v.offset + (page - v.start))};
      }
      at = v.end;
    }
    if (at < hi) return {"mirror", f("Local() has no mapping at [{:#x}, {:#x})", at, hi)};
    uint64_t live = 0;
    for (const Rec& r : s.regions) live += r.local.size();
    if (live) mirror_ratio = std::max(mirror_ratio, (double)s.mirror.size() / live);
    return {};
  }

  Failure CheckTranslation(const Snapshot& s) {
    uint64_t seed = rng.Next();
    Str error = [&]() -> Str {
      Rng sample{seed};
      auto expect_find = [&](uint64_t addr) -> Str {
        const Rec* want = RegionAt(s.regions, addr);
        const MemMeld::Region* got = mirror->mem_meld.Find(addr);
        uint64_t k = want ? addr - want->remote : 0, rest = want ? want->local.size() - k : 1;
        Span<char> local = mirror->mem_meld.ToLocal(addr, rest);
        if (!want) {
          if (got || !local.empty())
            return f(
                "Find({:#x}) returned {} and ToLocal {} bytes for an address outside every "
                "region",
                addr, got ? mirror->Describe(*got) : Str("null"), local.size());
          return {};
        }
        if (!got || got->remote != want->remote || got->local.data() != want->local.data() ||
            got->local.size() != want->local.size() || got->prot != want->prot ||
            got->file_offset != want->file_offset ||
            (profile.check_paths && got->path != want->path))
          return f("Find({:#x}) returned {} instead of {}", addr,
                   got ? mirror->Describe(*got) : Str("null"), FuzzMirror::Describe(*want));
        if (local.data() != want->local.data() + k || local.size() != rest)
          return f("ToLocal({:#x}) returned {} bytes at {} instead of {} bytes at {}", addr,
                   local.size(), (void*)local.data(), rest, (void*)(want->local.data() + k));
        if (Span<char> past = mirror->mem_meld.ToLocal(addr, rest + 1); !past.empty())
          return f("ToLocal({:#x}, {:#x}) returned {} bytes past the end of {}", addr, rest + 1,
                   past.size(), FuzzMirror::Describe(*want));
        if (uintptr_t back = mirror->mem_meld.ToRemote(want->local.data() + k); back != addr)
          return f("ToRemote({}) returned {:#x} instead of {:#x}", (void*)(want->local.data() + k),
                   back, addr);
        return {};
      };
      for (const Rec& r : s.regions) {
        uint64_t len = r.local.size();
        for (uint64_t addr : std::initializer_list<uint64_t>{
                 r.remote, r.remote + len - 1, r.remote + sample.Next() % len, r.remote - 1,
                 r.remote + len, r.remote + (1ull << 48)})
          if (Str e = expect_find(addr); !e.empty()) return e;
      }
      for (const Interval& in : s.intervals)
        if (!in.region)
          for (uint64_t addr : std::initializer_list<uint64_t>{
                   in.start, in.end - 1, in.start + sample.Next() % (in.end - in.start)})
            if (Str e = expect_find(addr); !e.empty()) return e;
      for (auto [remote, len] : s.removed)
        for (uint64_t addr : std::initializer_list<uint64_t>{remote, remote + len - 1,
                                                             remote + sample.Next() % len})
          if (Str e = expect_find(addr); !e.empty()) return e;
      Span<char> all = mirror->mem_meld.Local();
      if (all.data() != s.mirror.data() || all.size() != s.mirror.size())
        return f("Local() changed without an event");
      Vec<const Rec*> by_local;
      for (const Rec& r : s.regions) by_local.push_back(&r);
      std::sort(by_local.begin(), by_local.end(),
                [](const Rec* a, const Rec* b) { return a->local.data() < b->local.data(); });
      uint64_t pages = all.size() / kPage, stride = std::max<uint64_t>(1, pages / 200000);
      for (uint64_t page = 0; page < pages; page += stride) {
        const char* p = all.data() + page * kPage + sample.Next() % kPage;
        auto it = std::upper_bound(by_local.begin(), by_local.end(), p,
                                   [](const char* a, const Rec* r) { return a < r->local.data(); });
        uintptr_t want = 0;
        if (it != by_local.begin() &&
            p < (*std::prev(it))->local.data() + (*std::prev(it))->local.size())
          want = (*std::prev(it))->remote + (p - (*std::prev(it))->local.data());
        if (uintptr_t got = mirror->mem_meld.ToRemote(p); got != want)
          return f("ToRemote({}) returned {:#x} instead of {:#x}", (void*)p, got, want);
      }
      int on_stack = 0;
      for (const void* p : {(const void*)nullptr, (const void*)&on_stack,
                            (const void*)(all.data() + all.size()), (const void*)(all.data() - 1)})
        if (uintptr_t got = mirror->mem_meld.ToRemote(p); got)
          return f("ToRemote({}) returned {:#x} for an address outside Local()", p, got);
      return {};
    }();
    if (!error.empty()) return {"translation", error};
    return {};
  }

  Failure CheckContent(const Snapshot& s) {
    Vec<char> target;
    auto compare = [&](const Rec& r, const char* local, uint64_t remote, uint64_t len,
                       bool zero) -> Failure {
      for (uint64_t at = 0; at < len; at += 1 << 20) {
        uint64_t n = std::min<uint64_t>(len - at, 1 << 20);
        target.resize(n);
        if (!proc[kA].Read(remote + at, target.data(), n))
          return {"content", f("cannot read the target at {:#x} for {}", remote + at,
                               FuzzMirror::Describe(r))};
        for (uint64_t i = 0; i < n; ++i) {
          char want = zero ? 0 : local[at + i];
          if (target[i] != want)
            return {"content",
                    f("{} reads {:02x} at {:#x} but the target has {:02x}", FuzzMirror::Describe(r),
                      (uint8_t)want, remote + at + i, (uint8_t)target[i])};
        }
      }
      return {};
    };
    for (const Interval& in : s.intervals) {
      if (!in.region || !in.target || !in.local) continue;
      const Rec& r = *in.region;
      const char* local = r.local.data() + (in.start - r.remote);
      uint64_t len = in.end - in.start;
      Failure failure;
      if (IsMirrorMemfd(*in.target)) {
        uint64_t offset = in.target->offset + (in.start - in.target->start);
        ForEachData(memfd, offset, offset + len, [&](uint64_t data, uint64_t n) {
          if (!failure)
            failure = compare(r, local + (data - offset), in.start + (data - offset), n, false);
        });
      } else {
        failure = compare(r, local, in.start, len, false);
      }
      if (failure) return failure;
    }
    return {};
  }

  Failure CheckReference(const Snapshot& s) {
    Vec<char> a, b;
    auto compare = [&](uint64_t addr_a, uint64_t addr_b, uint64_t len, const Str& what) -> Failure {
      a.resize(len);
      b.resize(len);
      if (!ReadA(s, addr_a, a.data(), len))
        return {"differential", f("{}: the mirrored target has no readable mapping at [{:#x}, "
                                  "{:#x})",
                                  what, addr_a, addr_a + len)};
      if (!proc[kB].Read(addr_b, b.data(), len))
        return {"reference", f("{}: the reference target cannot be read at {:#x}", what, addr_b)};
      for (uint64_t i = 0; i < len; ++i)
        if (a[i] != b[i])
          return {"differential",
                  f("{}: byte {:#x} at {:#x} is {:02x} in the mirrored target but {:02x} in the "
                    "reference",
                    what, i, addr_a + i, (uint8_t)a[i], (uint8_t)b[i])};
      return {};
    };
    for (size_t index = 0; index < areas.size(); ++index) {
      const Area& area = areas[index];
      for (size_t i = 0; i < area.pages.size();) {
        if (!area.pages[i].mapped || !area.pages[i].defined) {
          ++i;
          continue;
        }
        size_t j = i;
        while (j < area.pages.size() && area.pages[j].mapped && area.pages[j].defined) ++j;
        Str what = f("pages [{}, {}) of the {} area created at step {}", i, j,
                     area.file  ? "file"
                     : area.brk ? "brk"
                                : "anonymous",
                     area.step);
        if (Failure failure = compare(area.base[kA] + i * kPage, area.base[kB] + i * kPage,
                                      (j - i) * kPage, what))
          return failure;
        i = j;
      }
    }
    for (const Block& block : blocks) {
      if (block.zeroed) {
        if (Failure failure = compare(block.addr[kA], block.addr[kB], block.len,
                                      f("calloc block of {} bytes", block.len)))
          return failure;
      }
      for (const Extent& e : block.writes)
        if (Failure failure = compare(block.addr[kA] + e.offset, block.addr[kB] + e.offset, e.len,
                                      f("write into a malloc block of {} bytes", block.len)))
          return failure;
    }
    return {};
  }

  Failure CheckLeaks(const Snapshot& s) {
    if (!profile.check_leaks) return {};
    Vec<std::pair<uint64_t, uint64_t>> used;
    for (const Vma& t : s.target)
      if (IsMirrorMemfd(t)) used.push_back({t.offset, t.offset + (t.end - t.start)});
    for (const Interval& in : s.intervals)
      if (in.region && in.local && IsMirrorMemfd(*in.local)) {
        uint64_t lo = in.local->offset + ((uint64_t)in.region->local.data() +
                                          (in.start - in.region->remote) - in.local->start);
        used.push_back({lo, lo + (in.end - in.start)});
      }
    std::sort(used.begin(), used.end());
    Vec<std::pair<uint64_t, uint64_t>> merged;
    for (auto [lo, hi] : used)
      if (!merged.empty() && lo <= merged.back().second)
        merged.back().second = std::max(merged.back().second, hi);
      else
        merged.push_back({lo, hi});
    uint64_t leaked = 0, first = 0;
    ForEachData(memfd, 0, 1ull << 60, [&](uint64_t data, uint64_t n) {
      auto it = std::upper_bound(merged.begin(), merged.end(), std::pair(data, UINT64_MAX));
      if (it != merged.begin() && std::prev(it)->second > data) --it;
      for (uint64_t at = data; at < data + n;) {
        if (it != merged.end() && it->first <= at) {
          at = std::max(at, it->second);
          ++it;
          continue;
        }
        uint64_t end = std::min(data + n, it == merged.end() ? UINT64_MAX : it->first);
        if (!leaked) first = at;
        leaked += (end - at) / kPage;
        at = end;
      }
    });
    if (leaked)
      return {"leak", f("{} allocated memfd pages are mapped by neither the target nor a region, "
                        "the first at offset {:#x}",
                        leaked, first)};
    return {};
  }
};

Op Generate(Rng& rng, const int (&weights)[kKinds], bool threads) {
  if (threads && rng.Next() % 3 == 0)
    return {Kind::kThreadsStop, rng.Next(), rng.Next(), rng.Next()};
  int total = 0;
  for (int w : weights) total += w;
  int pick = (int)(rng.Next() % total);
  int kind = 0;
  while (pick >= weights[kind]) pick -= weights[kind++];
  return {(Kind)kind, rng.Next(), rng.Next(), rng.Next()};
}

struct Outcome {
  Failure failure;
  Vec<Op> ops;
  double mirror_ratio = 0;
};

Outcome Fuzz(uint64_t seed, uint64_t steps, uint64_t check_every, const Profile& profile,
             bool trace) {
  Outcome out;
  Rng rng{seed};
  Run run(profile, trace);
  if ((out.failure = run.Start())) return out;
  auto step = [&](const Op& op, bool check) {
    out.ops.push_back(op);
    out.failure = run.Execute(op);
    if (!out.failure && check) out.failure = run.Check();
    return !out.failure;
  };
  for (uint64_t i = rng.Next() % 8; i > 0; --i)
    if (!step(Generate(rng, profile.before_attach, run.threads), false)) return out;
  if (!step({Kind::kAttach, rng.Next(), rng.Next(), rng.Next()}, true)) return out;
  for (uint64_t i = 1; i <= steps; ++i)
    if (!step(Generate(rng, profile.after_attach, run.threads), i % check_every == 0 || i == steps))
      return out;
  if (run.threads && !step({Kind::kThreadsStop, 0, 0, 0}, true)) return out;
  out.mirror_ratio = run.mirror_ratio;
  return out;
}

Failure Replay(const Vec<Op>& ops, const Profile& profile, bool trace,
               size_t* failed_at = nullptr) {
  Run run(profile, trace);
  if (Failure failure = run.Start()) return failure;
  for (size_t i = 0; i <= ops.size(); ++i) {
    Op op = i < ops.size() ? ops[i] : Op{Kind::kThreadsStop, 0, 0, 0};
    Failure failure = run.Execute(op);
    if (!failure) failure = run.Check();
    if (failure) {
      if (failed_at) *failed_at = std::min(i, ops.size() - 1);
      if (trace) Print(f("  failed: [{}] {}\n", failure.category, failure.detail));
      return failure;
    }
  }
  return {};
}

Vec<Op> Shrink(Vec<Op> ops, const Str& category, const Profile& profile) {
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::seconds(Env("MEM_MELD_FUZZ_SHRINK_SECONDS", 120));
  size_t at = 0;
  auto fails = [&](const Vec<Op>& candidate) {
    return Replay(candidate, profile, false, &at).category == category;
  };
  if (!fails(ops)) return ops;
  ops.resize(at + 1);
  for (size_t chunks = 2; ops.size() > 1 && std::chrono::steady_clock::now() < deadline;) {
    size_t chunk = (ops.size() + chunks - 1) / chunks;
    bool reduced = false;
    for (size_t start = 0; start < ops.size() && std::chrono::steady_clock::now() < deadline;
         start += chunk) {
      Vec<Op> candidate(ops.begin(), ops.begin() + start);
      candidate.insert(candidate.end(), ops.begin() + std::min(start + chunk, ops.size()),
                       ops.end());
      if (!candidate.empty() && fails(candidate)) {
        candidate.resize(at + 1);
        ops = std::move(candidate);
        reduced = true;
        break;
      }
    }
    if (reduced)
      chunks = std::max<size_t>(chunks - 1, 2);
    else if (chunks >= ops.size())
      break;
    else
      chunks = std::min(chunks * 2, ops.size());
  }
  return ops;
}

Str ReportFailure(uint64_t seed, const Outcome& outcome, const Profile& profile) {
  Str text = outcome.ops.empty() ? f("seed {} failed before the first step: [{}] {}\n", seed,
                                     outcome.failure.category, outcome.failure.detail)
                                 : f("seed {} failed at step {} {}: [{}] {}\n", seed,
                                     outcome.ops.size() - 1, Describe(outcome.ops.back()),
                                     outcome.failure.category, outcome.failure.detail);
  Print("mem_meld_fuzz: " + text);
  if (outcome.ops.empty() || !Env("MEM_MELD_FUZZ_SHRINK", 1)) return text;
  Vec<Op> minimal = Shrink(outcome.ops, outcome.failure.category, profile);
  Str repro = f("minimal reproduction of seed {} ({} operations):\n", seed, minimal.size());
  for (const Op& op : minimal) repro += "  " + Describe(op) + ",\n";
  Print(repro);
  Print("trace of the minimal reproduction:\n");
  Replay(minimal, profile, true);
  return text + repro;
}

void ExpectReplayPasses(const Vec<Op>& ops, bool check_paths = false, bool check_leaks = false) {
  Profile profile = FullProfile();
  profile.check_paths = check_paths;
  profile.check_leaks = check_leaks;
  Failure failure = Replay(ops, profile, Env("MEM_MELD_FUZZ_TRACE", 0));
  EXPECT_FALSE(failure) << "[" << failure.category << "] " << failure.detail;
}

struct MemMeldFuzz : ::testing::Test {
  static void SetUpTestSuite() {
    static bool initialized = false;
    if (initialized) return;
    signal(SIGPIPE, SIG_IGN);
    File();
    initialized = true;
  }
};

TEST_F(MemMeldFuzz, RandomSequences) {
  uint64_t first = Env("MEM_MELD_FUZZ_SEED", 1);
  uint64_t seeds = Env("MEM_MELD_FUZZ_SEEDS", 3);
  uint64_t steps = Env("MEM_MELD_FUZZ_STEPS", 300);
  uint64_t check_every = std::max<uint64_t>(1, Env("MEM_MELD_FUZZ_CHECK_EVERY", 1));
  bool trace = Env("MEM_MELD_FUZZ_TRACE", 0);
  Profile profile = FullProfile();
  double mirror_ratio = 0;
  for (uint64_t seed = first; seed < first + seeds; ++seed) {
    Outcome outcome = Fuzz(seed, steps, check_every, profile, trace);
    if (outcome.failure)
      ADD_FAILURE() << ReportFailure(seed, outcome, profile);
    else
      mirror_ratio = std::max(mirror_ratio, outcome.mirror_ratio);
  }
  Print(f("mem_meld_fuzz: seeds {}..{} done, largest Local() to live size ratio {:.2f}\n", first,
          first + seeds - 1, mirror_ratio));
}

TEST_F(MemMeldFuzz, Seed1_RegionPathDangles) {
  ExpectReplayPasses({{Kind::kAttach, 0xc34d0bff90150280, 0xe099ec6cd7363ca5, 0x85e7bb0f12278575}},
                     true);
}

TEST_F(MemMeldFuzz, Seed2_MremapMoveOfReadOnlyMemory) {
  ExpectReplayPasses(
      {{Kind::kMmap, 0x987bbcbfdd7e532f, 0xc3f2827affe7f664, 0x4fc446b53f17fb29},
       {Kind::kAttach, 0xbd80b4cb2ebfa252, 0x8675d8d1e5ceef8e, 0x9ea9566e32510720},
       {Kind::kMremapMove, 0x9839e745b98cdaa6, 0x99865fd6f4009b0d, 0x3b3a8762bfcb6581}});
}

TEST_F(MemMeldFuzz, Seed3_MremapOfReadOnlyMemory) {
  ExpectReplayPasses(
      {{Kind::kMmap, 0xb2ccb612d7d47acc, 0xb63e2d8f305c487f, 0x7aec0f999d0fdcf4},
       {Kind::kAttach, 0xef8a0d2ce00389c6, 0xab6dd5ff979d2193, 0x30ec2fb3344507d1},
       {Kind::kMremapWhole, 0xab6b778d04660b18, 0x60dc89dae2cce024, 0x5ea3f30dd3154cd0}});
}

TEST_F(MemMeldFuzz, Seed6_MremapMoveOfPartOfRegion) {
  ExpectReplayPasses(
      {{Kind::kAttach, 0x72419db23951df99, 0xe6c7d0372aa2f46, 0x1b049812edd0da90},
       {Kind::kMmap, 0xcf5b4b2ee1b156b6, 0xfce874620c5b3386, 0x2d9483f0a7b12eec},
       {Kind::kMremapMove, 0x7285f567acf08c14, 0x58811204930fff65, 0x2c0635930d731560}});
}

TEST_F(MemMeldFuzz, Seed10_MremapMoveAfterMprotectSplit) {
  ExpectReplayPasses(
      {{Kind::kMmap, 0x21876e7a2aec4a3d, 0xd77e91a249eb9308, 0xdb3658ea52921cc8},
       {Kind::kAttach, 0x4b0e102e5a42353d, 0x3db5c25e1831ec80, 0x49e36993528b0270},
       {Kind::kMprotect, 0x64732fe554f17208, 0x73f05146172d59c1, 0x15e2f51e7660cde8},
       {Kind::kMremapMove, 0x3743c091390aaea2, 0x1ed1d915d6891362, 0x86cf4828c1c7c685}});
}

TEST_F(MemMeldFuzz, Seed11_ThreadsRunningDuringAttach) {
  ExpectReplayPasses(
      {{Kind::kThreadsStart, 0xa356be306e9b126d, 0x812e6299272e6df0, 0x2a4a0f1a750459c4},
       {Kind::kAttach, 0xee4bd29256520e56, 0xdc19499cd69bda16, 0xbf7c142a2dc0071a}});
}

TEST_F(MemMeldFuzz, Seed14_MprotectOfAFileMapping) {
  ExpectReplayPasses(
      {{Kind::kMmapFile, 0xfc7a94c76201a969, 0xb0917a853b187b49, 0xb185b49180b0f0de},
       {Kind::kAttach, 0x8061143a3b55fbfc, 0x2c2486d890d3b9a3, 0xe3c465b4a5e44cf8},
       {Kind::kMprotect, 0x2d4aef4b5ca199a4, 0xbb74cad236f984b3, 0xf9899fd80275948e}});
}

TEST_F(MemMeldFuzz, Seed22_FileMappingAfterAttach) {
  ExpectReplayPasses(
      {{Kind::kAttach, 0xff2ee709c280bf44, 0x64126f5be79a57a7, 0xae194a3d8540d881},
       {Kind::kMmapFile, 0x7ad54011a2325aca, 0x9afffc25a0e7716, 0xeb905aa91ad90ff5}});
}

TEST_F(MemMeldFuzz, FileMappingsOfEveryProtectionAfterAttach) {
  ExpectReplayPasses({{Kind::kAttach, 0, 0, 0},
                      {Kind::kMmapFile, 7, 0, 0},
                      {Kind::kMmapFile, 9, 1, 0},
                      {Kind::kMmapFile, 11, 2, 0},
                      {Kind::kWrite, 1, 0, 0},
                      {Kind::kMprotect, 2, 0, 3},
                      {Kind::kMunmapWhole, 0, 0, 0}});
}

TEST_F(MemMeldFuzz, LibrariesLoadedAfterAttach) {
  ExpectReplayPasses({{Kind::kAttach, 0, 0, 0},
                      {Kind::kDlopen, 0, 0, 0},
                      {Kind::kDlopen, 1, 0, 0},
                      {Kind::kDlopen, 2, 0, 0},
                      {Kind::kDlopen, 3, 0, 0},
                      {Kind::kDlopen, 4, 0, 0}});
}

}  // namespace
