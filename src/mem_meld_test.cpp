// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "mem_meld.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <thread>

#include "format.hpp"
#include "gtest.hpp"
#include "path.hpp"
#include "thread_name.hpp"
#include "tracy_client.hpp"
#include "vec.hpp"

using namespace automat;

namespace {

constexpr uint64_t kPage = 4096;
constexpr int kMemfdDescriptor = 900;

struct Target {
  pid_t pid = 0;
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
    ASSERT_EQ(err, 0) << strerror(err);
    ASSERT_EQ(ReadLine().substr(0, 4), "pid ");
  }

  Str ReadLine() {
    for (;;) {
      if (size_t nl = buffer.find('\n'); nl != Str::npos) {
        Str line = buffer.substr(0, nl);
        buffer.erase(0, nl + 1);
        return line;
      }
      pollfd readable{from, POLLIN, 0};
      if (poll(&readable, 1, 20'000) != 1) return {};
      char chunk[256];
      ssize_t n = read(from, chunk, sizeof(chunk));
      if (n <= 0) return {};
      buffer.append(chunk, n);
    }
  }

  void Send(StrView line) {
    Str out(line);
    out += '\n';
    EXPECT_EQ(write(to, out.data(), out.size()), (ssize_t)out.size());
  }

  Str Command(StrView line) {
    Send(line);
    return ReadLine();
  }

  uintptr_t CommandAddr(StrView line) {
    Str reply = Command(line);
    EXPECT_EQ(reply.substr(0, 3), "ok ") << line << " -> " << reply;
    return strtoull(reply.c_str() + 3, nullptr, 16);
  }

  void Close() {
    if (to >= 0) close(to);
    if (from >= 0) close(from);
    to = from = -1;
  }
};

bool ReadRemote(pid_t pid, uintptr_t addr, void* buf, size_t len) {
  iovec l{buf, len}, r{(void*)addr, len};
  return process_vm_readv(pid, &l, 1, &r, 1, 0) == (ssize_t)len;
}

Str ReadFile(const Str& path) {
  int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  Str out;
  char buf[4096];
  for (ssize_t n; fd >= 0 && (n = read(fd, buf, sizeof buf)) > 0;) out.append(buf, n);
  if (fd >= 0) close(fd);
  return out;
}

std::set<int> Descriptors(pid_t pid) {
  std::set<int> out;
  if (DIR* d = opendir(f("/proc/{}/fd", (int)pid).c_str())) {
    while (dirent* e = readdir(d))
      if (e->d_name[0] != '.') out.insert(atoi(e->d_name));
    closedir(d);
  }
  return out;
}

struct Vma {
  uint64_t start, end, offset;
  Str perms, path;
  bool Memfd() const { return path.starts_with("/memfd:pm-mirror"); }
};

Vec<Vma> Mappings(pid_t pid, uint64_t lo = 0, uint64_t hi = ~0ull) {
  Str maps = ReadFile(f("/proc/{}/maps", (int)pid));
  Vec<Vma> out;
  for (StrView rest = maps; !rest.empty();) {
    Str line(rest.substr(0, rest.find('\n')));
    rest.remove_prefix(std::min(rest.size(), line.size() + 1));
    Vma v;
    char perms[8] = {};
    int name = 0;
    if (sscanf(line.c_str(), "%lx-%lx %7s %lx %*s %*s %n", &v.start, &v.end, perms, &v.offset,
               &name) < 4 ||
        v.end <= lo || v.start >= hi)
      continue;
    v.perms = perms;
    if (name > 0 && (size_t)name < line.size()) v.path = line.substr(name);
    out.push_back(v);
  }
  return out;
}

bool AnyMemfd(pid_t pid) {
  return std::ranges::any_of(Mappings(pid), [](const Vma& v) { return v.Memfd(); });
}

uint64_t MemfdBytes(pid_t pid) {
  struct stat st;
  return stat(f("/proc/{}/fd/{}", (int)pid, kMemfdDescriptor).c_str(), &st) == 0
             ? (uint64_t)st.st_blocks * 512
             : 0;
}

int TracerThreads() {
  int n = 0;
  if (DIR* d = opendir("/proc/self/task")) {
    while (dirent* e = readdir(d))
      n += e->d_name[0] != '.' && ReadFile(f("/proc/self/task/{}/comm", e->d_name)) == "MemMeld\n";
    closedir(d);
  }
  return n;
}

pid_t TracerPid(pid_t pid) {
  Str status = ReadFile(f("/proc/{}/status", (int)pid));
  size_t at = status.find("TracerPid:");
  return at == Str::npos ? -1 : atoi(status.c_str() + at + 10);
}

struct TestMirror {
  using Region = MemMeld::Region;

  struct Rec {
    uintptr_t remote;
    Span<char> local;
    int prot;
    Str path;
    Size file_offset;
  };

  std::mutex mutex;
  std::condition_variable cv;
  std::map<uintptr_t, Rec> live;
  Vec<Rec> history;
  Vec<std::pair<Rec, Rec>> changes;
  Vec<uintptr_t> unmapped;
  int events = 0;
  bool attached = false;
  int attachments = 0;
  bool detached = false;
  bool detached_ok = false;
  Str detached_status;
  int translation_errors = 0;
  bool follow = false;
  bool detach_on_attached = false;
  Vec<std::unique_ptr<TestMirror>> children;
  MemMeld mem_meld;
  std::thread thread;

  TestMirror() {
    mem_meld.on_attached = [this] { OnAttached(); };
    mem_meld.on_mapped = [this](const Region& r) { OnMapped(r); };
    mem_meld.on_changed = [this](const Region& before, const Region& after) {
      OnChanged(before, after);
    };
    mem_meld.on_unmapped = [this](const Region& r) { OnUnmapped(r); };
    mem_meld.on_forked = [this](int pidfd) { OnForked(pidfd); };
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

  static Rec Copy(const Region& r) {
    return {r.remote, r.local, r.prot, Str(r.path), r.file_offset};
  }

  void Record(const Region& r) {
    live[r.remote] = Copy(r);
    history.push_back(Copy(r));
    const Region* found = mem_meld.Find(r.remote + r.local.size() / 2);
    uint64_t last = r.local.size() - 1;
    translation_errors += !found || found->remote != r.remote ||
                          mem_meld.ToLocal(r.remote, r.local.size()).data() != r.local.data() ||
                          mem_meld.ToLocal(r.remote + last, 1).size() != 1 ||
                          !mem_meld.ToLocal(r.remote, r.local.size() + 1).empty() ||
                          mem_meld.ToRemote(r.local.data() + last) != r.remote + last;
    cv.notify_all();
  }

  void OnAttached() {
    std::lock_guard lock(mutex);
    ++events;
    attached = true;
    ++attachments;
    cv.notify_all();
    if (detach_on_attached) mem_meld.Detach();
  }
  void OnMapped(const Region& r) {
    std::lock_guard lock(mutex);
    ++events;
    Record(r);
  }
  void OnChanged(const Region& before, const Region& after) {
    std::lock_guard lock(mutex);
    ++events;
    changes.push_back({Copy(before), Copy(after)});
    live.erase(before.remote);
    Record(after);
  }
  void OnUnmapped(const Region& r) {
    std::lock_guard lock(mutex);
    ++events;
    live.erase(r.remote);
    unmapped.push_back(r.remote);
    cv.notify_all();
  }
  void OnForked(int pidfd) {
    std::lock_guard lock(mutex);
    ++events;
    if (!follow) return;
    children.push_back(std::make_unique<TestMirror>());
    children.back()->Attach(pidfd);
  }
  void OnDetached(const Status& status) {
    std::lock_guard lock(mutex);
    ++events;
    detached = true;
    detached_ok = OK(status);
    detached_status = status.ToStr();
    cv.notify_all();
  }

  int Events() {
    std::lock_guard lock(mutex);
    return events;
  }

  bool WaitAttached() {
    std::unique_lock lock(mutex);
    return cv.wait_for(lock, std::chrono::seconds(20), [&] { return attached || detached; }) &&
           attached;
  }
  bool WaitAttachments(int n) {
    std::unique_lock lock(mutex);
    cv.wait_for(lock, std::chrono::seconds(20), [&] { return attachments >= n || detached; });
    return attachments >= n;
  }
  bool WaitDetached() {
    std::unique_lock lock(mutex);
    return cv.wait_for(lock, std::chrono::seconds(20), [&] { return detached; });
  }

  Span<char> Local(uintptr_t remote) {
    std::lock_guard lock(mutex);
    auto it = live.upper_bound(remote);
    if (it == live.begin() || remote >= (--it)->first + it->second.local.size()) return {};
    return it->second.local.subspan(remote - it->first);
  }

  uintptr_t Base(uintptr_t remote) {
    Span<char> local = Local(remote);
    std::lock_guard lock(mutex);
    for (auto& [base, rec] : live)
      if (rec.local.data() + rec.local.size() == local.data() + local.size() && !local.empty())
        return base;
    return 0;
  }

  void DetachAll() {
    Detach();
    for (auto& child : children) child->Detach();
  }
};

struct Fixture : ::testing::Test {
  Target target;
  std::unique_ptr<TestMirror> mirror;

  static void SetUpTestSuite() { signal(SIGPIPE, SIG_IGN); }

  void SetUp() override { target.Spawn(); }

  void StartAttach() {
    int pidfd = (int)syscall(SYS_pidfd_open, target.pid, 0);
    ASSERT_GE(pidfd, 0);
    mirror = std::make_unique<TestMirror>();
    mirror->Attach(pidfd);
    close(pidfd);
  }

  void Attach() {
    ASSERT_NO_FATAL_FAILURE(StartAttach());
    ASSERT_TRUE(mirror->WaitAttached()) << mirror->detached_status;
  }

  void Destroy() {
    mirror->DetachAll();
    mirror.reset();
  }

  void ExpectContentMatches() {
    std::lock_guard lock(mirror->mutex);
    for (auto& [remote, rec] : mirror->live) {
      Vec<char> remote_bytes(rec.local.size());
      if (!ReadRemote(target.pid, remote, remote_bytes.data(), remote_bytes.size())) continue;
      if (memcmp(remote_bytes.data(), rec.local.data(), rec.local.size()) != 0) {
        size_t off = 0;
        while (off < rec.local.size() && remote_bytes[off] == rec.local[off]) ++off;
        ADD_FAILURE() << "content mismatch at " << f("{:x}", remote) << "+" << f("{:x}", off)
                      << " prot=" << rec.prot << " foff=" << f("{:x}", rec.file_offset) << " ("
                      << rec.path << ") remote=" << f("{:02x}", (uint8_t)remote_bytes[off])
                      << " local=" << f("{:02x}", (uint8_t)rec.local[off]);
      }
    }
  }

  void ExpectRegionsMapped() {
    std::lock_guard lock(mirror->mutex);
    for (auto& [remote, rec] : mirror->live)
      EXPECT_FALSE(Mappings(target.pid, remote, remote + rec.local.size()).empty())
          << f("region {:x}", remote);
  }

  int Reap() {
    int status = -1;
    pollfd exited{(int)syscall(SYS_pidfd_open, target.pid, 0), POLLIN, 0};
    if (poll(&exited, 1, 20'000) == 1) waitpid(target.pid, &status, 0);
    close(exited.fd);
    target.pid = 0;
    return status;
  }

  bool Covered(uintptr_t remote) {
    std::lock_guard lock(mirror->mutex);
    for (auto& [base, rec] : mirror->live)
      if (remote >= base && remote < base + rec.local.size()) return true;
    return false;
  }

  Size RegionSize(uintptr_t remote) {
    std::lock_guard lock(mirror->mutex);
    auto it = mirror->live.find(remote);
    return it == mirror->live.end() ? 0 : it->second.local.size();
  }

  bool MirrorMatchesRemote(uintptr_t remote, size_t len) {
    Span<char> local = mirror->Local(remote);
    if (local.size() < len) return false;
    Vec<char> remote_bytes(len);
    if (!ReadRemote(target.pid, remote, remote_bytes.data(), len)) return false;
    return memcmp(remote_bytes.data(), local.data(), len) == 0;
  }

  bool MirrorAllZero(uintptr_t remote, size_t len) {
    Span<char> local = mirror->Local(remote);
    if (local.size() < len) return false;
    for (size_t i = 0; i < len; ++i)
      if (local[i] != 0) return false;
    return true;
  }

  Vma LocalMapping(uintptr_t remote) {
    Span<char> local = mirror->Local(remote);
    Vec<Vma> vmas = Mappings(getpid(), (uint64_t)local.data(), (uint64_t)local.data() + 1);
    return vmas.empty() ? Vma{} : vmas[0];
  }

  void TearDown() override {
    if (mirror) {
      EXPECT_EQ(mirror->translation_errors, 0);
      Destroy();
    }
    target.Close();
    if (target.pid) {
      kill(target.pid, SIGKILL);
      waitpid(target.pid, nullptr, 0);
    }
  }
};

TEST_F(Fixture, AttachAndDetach) {
  Attach();
  EXPECT_FALSE(mirror->live.empty());
  target.Close();
  EXPECT_TRUE(mirror->WaitDetached());
  EXPECT_TRUE(mirror->detached_ok);
}

TEST_F(Fixture, InitialSnapshotMatchesTarget) {
  Attach();
  ExpectContentMatches();
  uintptr_t heap = target.CommandAddr("malloc 64");
  EXPECT_TRUE(Covered(heap));
}

TEST_F(Fixture, LiveReadSeesTargetWrites) {
  Attach();
  uintptr_t addr = target.CommandAddr("mmap 65536 rw");
  EXPECT_EQ(target.Command(f("write {:#x} 4096 7", addr)), "ok");
  EXPECT_TRUE(MirrorMatchesRemote(addr, 4096));
}

TEST_F(Fixture, LiveWriteReachesTarget) {
  Attach();
  uintptr_t addr = target.CommandAddr("mmap 65536 rw");
  EXPECT_EQ(target.Command(f("write {:#x} 4096 11", addr)), "ok");
  Span<char> local = mirror->Local(addr);
  ASSERT_GE(local.size(), 4096u);
  local[100] = (char)0xA5;
  uint8_t observed = 0;
  ASSERT_TRUE(ReadRemote(target.pid, addr + 100, &observed, 1));
  EXPECT_EQ(observed, 0xA5);
}

TEST_F(Fixture, LocalProtection) {
  Attach();
  uintptr_t addr = target.CommandAddr("mmap 4096 r");
  Span<char> local = mirror->Local(addr);
  ASSERT_GE(local.size(), 4096u);
  local[0] = 'M';
  char observed = 0;
  ASSERT_TRUE(ReadRemote(target.pid, addr, &observed, 1));
  EXPECT_EQ(observed, 'M');
  uintptr_t file = target.CommandAddr("mmapfile /etc/hostname 4096 r");
  EXPECT_EQ(LocalMapping(file).perms.substr(0, 2), "r-");
  EXPECT_EQ(LocalMapping(file).path, "/etc/hostname");
}

TEST_F(Fixture, RawMmapIsMirrored) {
  Attach();
  uintptr_t addr = target.CommandAddr("rawmmap 65536 rw");
  EXPECT_EQ(target.Command(f("write {:#x} 65536 17", addr)), "ok");
  EXPECT_TRUE(Mappings(target.pid, addr, addr + 1)[0].Memfd());
  EXPECT_TRUE(MirrorMatchesRemote(addr, 65536));
}

TEST_F(Fixture, RawMmapKeepsTheArgumentRegisters) {
  Attach();
  EXPECT_EQ(target.Command("rawmmapregisters"), "ok");
}

TEST_F(Fixture, RawMadviseKeepsTheArgumentRegisters) {
  Attach();
  uintptr_t addr = target.CommandAddr("mmap 65536 rw");
  EXPECT_EQ(target.Command(f("rawmadviseregisters {:#x} 65536", addr)), "ok");
}

TEST_F(Fixture, MmapAndMunmapWhole) {
  Attach();
  uintptr_t addr = target.CommandAddr("mmap 65536 rw");
  EXPECT_TRUE(Covered(addr));
  EXPECT_EQ(target.Command(f("munmap {:#x} 65536", addr)), "ok");
  EXPECT_FALSE(Covered(addr));
}

TEST_F(Fixture, MunmapMiddleSplits) {
  Attach();
  uintptr_t addr = target.CommandAddr("mmap 12288 rw");
  EXPECT_EQ(target.Command(f("write {:#x} 12288 5", addr)), "ok");
  EXPECT_EQ(target.Command(f("munmap {:#x} 4096", addr + 4096)), "ok");
  EXPECT_TRUE(Covered(addr));
  EXPECT_FALSE(Covered(addr + 4096));
  EXPECT_TRUE(Covered(addr + 8192));
  EXPECT_TRUE(MirrorMatchesRemote(addr, 4096));
  EXPECT_TRUE(MirrorMatchesRemote(addr + 8192, 4096));
}

TEST_F(Fixture, MremapGrowKeepsContentTailZero) {
  Attach();
  uintptr_t addr = target.CommandAddr("mmap 65536 rw");
  EXPECT_EQ(target.Command(f("write {:#x} 65536 9", addr)), "ok");
  uintptr_t grown = target.CommandAddr(f("mremap {:#x} 65536 131072", addr));
  EXPECT_TRUE(MirrorMatchesRemote(grown, 65536));
  EXPECT_TRUE(MirrorAllZero(grown + 65536, 65536));
}

TEST_F(Fixture, MremapShrink) {
  Attach();
  uintptr_t addr = target.CommandAddr("mmap 131072 rw");
  EXPECT_EQ(target.Command(f("write {:#x} 131072 3", addr)), "ok");
  uint64_t before = MemfdBytes(target.pid);
  uintptr_t shrunk = target.CommandAddr(f("mremap {:#x} 131072 65536", addr));
  EXPECT_EQ(RegionSize(shrunk), 65536u);
  EXPECT_LT(MemfdBytes(target.pid), before - 32768);
}

TEST_F(Fixture, MremapMovePreservesContent) {
  Attach();
  uintptr_t addr = target.CommandAddr("mmap 65536 rw");
  EXPECT_EQ(target.Command(f("write {:#x} 65536 13", addr)), "ok");
  uintptr_t moved = target.CommandAddr(f("mremapmove {:#x} 65536 65536", addr));
  EXPECT_FALSE(Covered(addr));
  EXPECT_TRUE(Covered(moved));
  EXPECT_TRUE(MirrorMatchesRemote(moved, 65536));
}

TEST_F(Fixture, MremapMoveKeepsTheRegion) {
  Attach();
  uintptr_t file = target.CommandAddr("mmapfile /etc/hostname 4096 r");
  uintptr_t anon = target.CommandAddr("mmap 16384 rw");
  EXPECT_EQ(target.Command(f("write {:#x} 16384 4", anon)), "ok");
  for (auto [addr, size, grown] : {std::tuple<uintptr_t, Size, Size>{file, 4096, 4096},
                                   std::tuple<uintptr_t, Size, Size>{anon, 16384, 32768}}) {
    Span<char> local = mirror->Local(addr);
    size_t changes, unmapped;
    {
      std::lock_guard lock(mirror->mutex);
      changes = mirror->changes.size();
      unmapped = mirror->unmapped.size();
    }
    uintptr_t moved = target.CommandAddr(f("mremapmove {:#x} {} {}", addr, size, grown));
    {
      std::lock_guard lock(mirror->mutex);
      EXPECT_EQ(std::count(mirror->unmapped.begin() + unmapped, mirror->unmapped.end(), addr), 0);
      ASSERT_EQ(mirror->changes.size(), changes + (grown > size ? 2 : 1));
      auto& [before, after] = mirror->changes[changes];
      EXPECT_EQ(before.remote, addr);
      EXPECT_EQ(after.remote, moved);
      EXPECT_EQ(after.local.data(), local.data());
      EXPECT_EQ(after.local.size(), size);
      EXPECT_EQ(after.prot, before.prot);
      EXPECT_EQ(after.path, addr == file ? "/etc/hostname" : "");
      EXPECT_EQ(after.file_offset, before.file_offset);
      EXPECT_EQ(mirror->changes.back().second.remote, moved);
      EXPECT_EQ(mirror->changes.back().second.local.size(), grown);
    }
    EXPECT_TRUE(MirrorMatchesRemote(moved, size));
  }
}

TEST_F(Fixture, MremapOfASubRangeMovesThatPart) {
  Attach();
  uintptr_t addr = target.CommandAddr("mmap 12288 rw");
  EXPECT_EQ(target.Command(f("write {:#x} 12288 3", addr)), "ok");
  uintptr_t moved = target.CommandAddr(f("mremapmove {:#x} 4096 4096", addr + 4096));
  EXPECT_TRUE(Covered(addr));
  EXPECT_FALSE(Covered(addr + 4096));
  EXPECT_TRUE(Covered(addr + 8192));
  EXPECT_TRUE(MirrorMatchesRemote(moved, 4096));
  ExpectContentMatches();
}

TEST_F(Fixture, MremapShrinkAcrossMappingsReleasesThem) {
  Attach();
  uintptr_t r = target.CommandAddr("mmap 16384 none");
  EXPECT_EQ(target.CommandAddr(f("mmap 8192 rw {:#x}", r)), r);
  EXPECT_EQ(target.CommandAddr(f("mmap 8192 rw {:#x}", r + 8192)), r + 8192);
  EXPECT_EQ(target.Command(f("write {:#x} 16384 8", r)), "ok");
  uint64_t before = MemfdBytes(target.pid);
  EXPECT_EQ(target.CommandAddr(f("mremap {:#x} 16384 4096", r)), r);
  EXPECT_LT(MemfdBytes(target.pid), before - 8192);
  EXPECT_EQ(RegionSize(r), 4096u);
  EXPECT_FALSE(Covered(r + 8192));
  uintptr_t next = target.CommandAddr(f("mmap 8192 rw {:#x}", r + 8192));
  EXPECT_EQ(target.Command(f("write {:#x} 8192 9", next)), "ok");
  EXPECT_TRUE(MirrorMatchesRemote(r, 4096));
  EXPECT_TRUE(MirrorMatchesRemote(next, 8192));
}

TEST_F(Fixture, MremapFixedReleasesTheDestination) {
  Attach();
  uintptr_t a = target.CommandAddr("mmap 1048576 rw");
  EXPECT_EQ(target.Command(f("write {:#x} 1048576 2", a)), "ok");
  uintptr_t b = target.CommandAddr("mmap 4096 rw");
  EXPECT_EQ(target.Command(f("write {:#x} 4096 4", b)), "ok");
  uint64_t before = MemfdBytes(target.pid);
  EXPECT_EQ(target.CommandAddr(f("mremapfixed {:#x} 4096 1048576 {:#x}", b, a)), a);
  EXPECT_LT(MemfdBytes(target.pid), before - 1048576 + 65536);
  EXPECT_EQ(target.Command(f("verify {:#x} 4096 4", a)), "ok");
  EXPECT_TRUE(MirrorMatchesRemote(a, 4096));
  EXPECT_TRUE(MirrorAllZero(a + 4096, 1048576 - 4096));
  EXPECT_FALSE(Covered(b));
}

TEST_F(Fixture, MremapDontunmapIsRefused) {
  Attach();
  uintptr_t addr = target.CommandAddr("mmap 4096 rw");
  EXPECT_EQ(target.Command(f("mremapdontunmap {:#x} 4096", addr)), "err EINVAL mremap");
  EXPECT_EQ(target.Command(f("write {:#x} 4096 6", addr)), "ok");
}

TEST_F(Fixture, MprotectSubRangeSplits) {
  Attach();
  uintptr_t addr = target.CommandAddr("mmap 12288 rw");
  EXPECT_EQ(target.Command(f("mprotect {:#x} 4096 r", addr + 4096)), "ok");
  std::lock_guard lock(mirror->mutex);
  ASSERT_TRUE(mirror->live.count(addr));
  ASSERT_TRUE(mirror->live.count(addr + 4096));
  ASSERT_TRUE(mirror->live.count(addr + 8192));
  EXPECT_EQ(mirror->live[addr].prot & PROT_WRITE, PROT_WRITE);
  EXPECT_EQ(mirror->live[addr + 4096].prot & PROT_WRITE, 0);
  EXPECT_EQ(mirror->live[addr + 8192].prot & PROT_WRITE, PROT_WRITE);
}

TEST_F(Fixture, MprotectAcrossAnInaccessiblePage) {
  uintptr_t addr = target.CommandAddr("mmap 8192 rw");
  EXPECT_EQ(target.Command(f("write {:#x} 4096 3", addr)), "ok");
  EXPECT_EQ(target.Command(f("mprotect {:#x} 4096 r", addr)), "ok");
  EXPECT_EQ(target.Command(f("mprotect {:#x} 4096 none", addr + 4096)), "ok");
  Attach();
  EXPECT_TRUE(Covered(addr));
  EXPECT_FALSE(Covered(addr + 4096));
  for (const Vma& v : Mappings(target.pid, addr, addr + 8192)) EXPECT_TRUE(v.Memfd());
  EXPECT_EQ(target.Command(f("mprotect {:#x} 8192 rw", addr)), "ok");
  EXPECT_EQ(target.Command(f("verify {:#x} 4096 3", addr)), "ok");
  EXPECT_EQ(target.Command(f("write {:#x} 8192 4", addr)), "ok");
  EXPECT_TRUE(MirrorMatchesRemote(addr, 4096));
  EXPECT_TRUE(MirrorMatchesRemote(addr + 4096, 4096));
}

TEST_F(Fixture, InaccessibleBytesSurviveBeingMadeAccessible) {
  uintptr_t addr = target.CommandAddr("mmap 8192 rw");
  EXPECT_EQ(target.Command(f("write {:#x} 8192 8", addr)), "ok");
  EXPECT_EQ(target.Command(f("mprotect {:#x} 8192 none", addr)), "ok");
  Attach();
  EXPECT_EQ(target.Command(f("mprotect {:#x} 8192 rw", addr)), "ok");
  EXPECT_EQ(target.Command(f("verify {:#x} 8192 8", addr)), "ok");
  EXPECT_TRUE(MirrorMatchesRemote(addr, 8192));
}

TEST_F(Fixture, ReservationsAreMemfdFromBirth) {
  Attach();
  uintptr_t addr = target.CommandAddr("mmap 67108864 none");
  EXPECT_TRUE(Mappings(target.pid, addr, addr + 1)[0].Memfd());
  EXPECT_FALSE(Covered(addr));
  EXPECT_EQ(target.Command(f("mprotect {:#x} 65536 rw", addr + 65536)), "ok");
  EXPECT_EQ(target.Command(f("write {:#x} 65536 12", addr + 65536)), "ok");
  EXPECT_TRUE(MirrorMatchesRemote(addr + 65536, 65536));
  EXPECT_EQ(target.Command(f("mprotect {:#x} 65536 none", addr + 65536)), "ok");
  EXPECT_FALSE(Covered(addr + 65536));
}

TEST_F(Fixture, UnmappedMemfdOffsetsAreReused) {
  Attach();
  std::set<uint64_t> offsets;
  for (int i = 0; i < 100; ++i) {
    uintptr_t addr = target.CommandAddr("mmap 65536 rw");
    auto mappings = Mappings(target.pid, addr, addr + 65536);
    ASSERT_EQ(mappings.size(), 1u);
    offsets.insert(mappings[0].offset);
    EXPECT_EQ(target.Command(f("munmap {:#x} 65536", addr)), "ok");
  }
  EXPECT_LE(offsets.size(), 2u);
}

TEST_F(Fixture, PiecesOfAReservationMergeAgain) {
  Attach();
  uintptr_t addr = target.CommandAddr("mmap 67108864 none");
  for (int i = 0; i < 8; ++i)
    EXPECT_EQ(target.Command(f("mprotect {:#x} 131072 rw", addr + i * 131072)), "ok");
  EXPECT_EQ(Mappings(target.pid, addr, addr + 8 * 131072).size(), 1u);
  for (int i = 0; i < 8; ++i) EXPECT_TRUE(MirrorMatchesRemote(addr + i * 131072, 131072));
}

TEST_F(Fixture, MremapGrowthAfterASplitSharesNoPages) {
  Attach();
  uintptr_t addr = target.CommandAddr("mmap 12288 rw");
  EXPECT_EQ(target.Command(f("write {:#x} 12288 5", addr)), "ok");
  EXPECT_EQ(target.Command(f("munmap {:#x} 4096", addr + 4096)), "ok");
  char upper[4096], upper_after[4096], tail[8192];
  ASSERT_TRUE(ReadRemote(target.pid, addr + 8192, upper, sizeof upper));
  uintptr_t grown = target.CommandAddr(f("mremap {:#x} 4096 12288", addr));
  ASSERT_TRUE(ReadRemote(target.pid, grown + 4096, tail, sizeof tail));
  EXPECT_TRUE(std::all_of(tail, tail + sizeof tail, [](char c) { return c == 0; }));
  EXPECT_EQ(target.Command(f("write {:#x} 8192 6", grown + 4096)), "ok");
  ASSERT_TRUE(ReadRemote(target.pid, addr + 8192, upper_after, sizeof upper_after));
  EXPECT_EQ(memcmp(upper, upper_after, sizeof upper), 0);
  EXPECT_TRUE(MirrorMatchesRemote(grown, 12288));
}

TEST_F(Fixture, MremapGrowthToFreshOffsetsKeepsTheRegion) {
  Attach();
  uintptr_t addr = target.CommandAddr("mmap 8192 rw");
  EXPECT_EQ(target.Command(f("write {:#x} 8192 7", addr)), "ok");
  uintptr_t moved = target.CommandAddr(f("mremapmove {:#x} 4096 4096", addr + 4096));
  uint64_t offset = Mappings(target.pid, addr, addr + 1)[0].offset;
  Span<char> local = mirror->Local(addr);
  size_t changes, unmapped;
  {
    std::lock_guard lock(mirror->mutex);
    changes = mirror->changes.size();
    unmapped = mirror->unmapped.size();
  }
  EXPECT_EQ(target.CommandAddr(f("mremap {:#x} 4096 8192", addr)), addr);
  EXPECT_NE(Mappings(target.pid, addr, addr + 1)[0].offset, offset);
  {
    std::lock_guard lock(mirror->mutex);
    EXPECT_EQ(mirror->unmapped.size(), unmapped);
    ASSERT_EQ(mirror->changes.size(), changes + 1);
    auto& [before, after] = mirror->changes.back();
    EXPECT_EQ(before.remote, addr);
    EXPECT_EQ(before.local.data(), local.data());
    EXPECT_EQ(after.remote, addr);
    EXPECT_EQ(after.local.size(), 8192u);
  }
  EXPECT_EQ(target.Command(f("verify {:#x} 4096 7", addr)), "ok");
  EXPECT_TRUE(MirrorMatchesRemote(addr, 8192));
  EXPECT_TRUE(MirrorMatchesRemote(moved, 4096));
}

TEST_F(Fixture, MremapOfSeveralMappingsIsRefused) {
  Attach();
  uintptr_t addr = target.CommandAddr("mmap 12288 rw");
  EXPECT_EQ(target.Command(f("write {:#x} 12288 5", addr)), "ok");
  EXPECT_EQ(target.Command(f("mprotect {:#x} 4096 r", addr + 4096)), "ok");
  EXPECT_EQ(target.Command(f("mremap {:#x} 8192 16384", addr)), "err EFAULT mremap");
  Vec<Vma> vmas = Mappings(target.pid, addr, addr + 12288);
  ASSERT_EQ(vmas.size(), 3u);
  EXPECT_EQ(vmas[1].perms.substr(0, 2), "r-");
  EXPECT_EQ(target.Command(f("verify {:#x} 12288 5", addr)), "ok");
  ExpectContentMatches();
}

TEST_F(Fixture, MremapGrowthBeyondASlotFailsWithEnomem) {
  Attach();
  uint64_t gib = 1ull << 30;
  uintptr_t addr = target.CommandAddr(f("mmap {} rw", 32 * gib));
  EXPECT_EQ(target.Command(f("mremap {:#x} {} {}", addr, 32 * gib, 100 * gib)),
            "err ENOMEM mremap");
  EXPECT_EQ(target.Command(f("write {:#x} 4096 2", addr)), "ok");
  EXPECT_TRUE(MirrorMatchesRemote(addr, 4096));
}

TEST_F(Fixture, BrkGrowAndShrink) {
  Attach();
  uintptr_t base = target.CommandAddr("brk 0");
  uintptr_t grown = target.CommandAddr("brk 131072");
  EXPECT_EQ(target.Command(f("write {:#x} 4096 1", grown - 4096)), "ok");
  EXPECT_GT(grown, base);
  EXPECT_TRUE(Covered(grown - 4096));
  EXPECT_TRUE(MirrorMatchesRemote(grown - 4096, 4096));
  Vec<Vma> heap = Mappings(target.pid, grown - 4096, grown);
  ASSERT_EQ(heap.size(), 1u);
  EXPECT_TRUE(heap[0].Memfd());
  EXPECT_LT(heap[0].start, base - 4096);
  target.CommandAddr("brk -131072");
  EXPECT_FALSE(Covered(grown - 4096));
  uintptr_t regrown = target.CommandAddr("brk 131072");
  EXPECT_TRUE(MirrorAllZero(regrown - 4096, 4096));
}

TEST_F(Fixture, BrkQueriesDuringGrowthKeepTheHeap) {
  Attach();
  EXPECT_EQ(target.Command("brkrace 3000"), "ok");
  ExpectContentMatches();
}

TEST_F(Fixture, MadviseZeroFill) {
  Attach();
  uintptr_t addr = target.CommandAddr("mmap 65536 rw");
  EXPECT_EQ(target.Command(f("write {:#x} 65536 21", addr)), "ok");
  EXPECT_TRUE(MirrorMatchesRemote(addr, 65536));
  EXPECT_EQ(target.Command(f("madvise {:#x} 65536 dontneed", addr)), "ok");
  EXPECT_TRUE(MirrorAllZero(addr, 65536));
}

TEST_F(Fixture, MadviseFreeZeroFills) {
  Attach();
  uintptr_t addr = target.CommandAddr("mmap 65536 rw");
  EXPECT_EQ(target.Command(f("write {:#x} 65536 22", addr)), "ok");
  EXPECT_EQ(target.Command(f("madvise {:#x} 65536 free", addr)), "ok");
  EXPECT_TRUE(MirrorAllZero(addr, 65536));
  char bytes[65536];
  ASSERT_TRUE(ReadRemote(target.pid, addr, bytes, sizeof bytes));
  EXPECT_TRUE(std::all_of(bytes, bytes + sizeof bytes, [](char c) { return c == 0; }));
}

TEST_F(Fixture, MadviseDontneedLockedZeroFills) {
  Attach();
  uintptr_t addr = target.CommandAddr("mmap 65536 rw");
  EXPECT_EQ(target.Command(f("write {:#x} 65536 23", addr)), "ok");
  EXPECT_EQ(target.Command(f("madvise {:#x} 65536 dontneedlocked", addr)), "ok");
  EXPECT_TRUE(MirrorAllZero(addr, 65536));
  char bytes[65536];
  ASSERT_TRUE(ReadRemote(target.pid, addr, bytes, sizeof bytes));
  EXPECT_TRUE(std::all_of(bytes, bytes + sizeof bytes, [](char c) { return c == 0; }));
}

TEST_F(Fixture, Translation) {
  Attach();
  uintptr_t addr = target.CommandAddr("mmap 65536 rw");
  EXPECT_EQ(mirror->Base(addr + 4096), addr);
  EXPECT_EQ(mirror->Local(addr + 65536 - 16).size(), 16u);
  EXPECT_EQ(mirror->Base(0x10000), 0u);
  EXPECT_EQ(mirror->translation_errors, 0);
}

TEST_F(Fixture, TranslationOfPiecesMovesAndShrinks) {
  Attach();
  MemMeld& mem_meld = mirror->mem_meld;
  uintptr_t addr = target.CommandAddr("mmap 12288 rw");
  EXPECT_EQ(target.Command(f("mprotect {:#x} 4096 r", addr + 4096)), "ok");
  for (uintptr_t piece : {addr, addr + 4096, addr + 8192}) {
    Span<char> local = mirror->Local(piece);
    ASSERT_EQ(local.size(), 4096u);
    EXPECT_EQ(mem_meld.ToLocal(piece, 4096).data(), local.data());
    EXPECT_EQ(mem_meld.ToRemote(local.data() + 4095), piece + 4095);
    EXPECT_TRUE(mem_meld.ToLocal(piece + 4095, 2).empty());
  }
  EXPECT_TRUE(mem_meld.ToLocal(addr, 12288).empty());
  EXPECT_TRUE(mem_meld.ToLocal(addr, 0).empty());

  Span<char> tail = mirror->Local(addr + 8192);
  uintptr_t moved = target.CommandAddr(f("mremapmove {:#x} 4096 4096", addr + 8192));
  EXPECT_TRUE(mem_meld.ToLocal(addr + 8192, 1).empty());
  EXPECT_EQ(mem_meld.ToLocal(moved, 4096).data(), tail.data());
  EXPECT_EQ(mem_meld.ToRemote(tail.data() + 17), moved + 17);

  uintptr_t big = target.CommandAddr("mmap 16384 rw");
  Span<char> big_local = mirror->Local(big);
  EXPECT_EQ(target.CommandAddr(f("mremap {:#x} 16384 4096", big)), big);
  EXPECT_EQ(mem_meld.ToLocal(big, 4096).data(), big_local.data());
  EXPECT_TRUE(mem_meld.ToLocal(big, 4097).empty());
  EXPECT_TRUE(mem_meld.ToLocal(big + 4096, 1).empty());
  EXPECT_EQ(mem_meld.ToRemote(big_local.data() + 4095), big + 4095);
  EXPECT_EQ(mem_meld.ToRemote(big_local.data() + 4096), 0u);
}

TEST_F(Fixture, TranslationAtTheTopOfTheAddressSpace) {
  constexpr uintptr_t kTop = 0x7ffffffff000;
  if (!Mappings(target.pid, kTop - 8192, kTop).empty()) GTEST_SKIP() << "the top pages are used";
  Attach();
  MemMeld& mem_meld = mirror->mem_meld;
  EXPECT_EQ(target.CommandAddr(f("mmap 8192 rw {:#x}", kTop - 8192)), kTop - 8192);
  Span<char> local = mirror->Local(kTop - 8192);
  ASSERT_EQ(local.size(), 8192u);
  EXPECT_EQ(mem_meld.ToLocal(kTop - 8192, 8192).data(), local.data());
  EXPECT_EQ(mem_meld.ToLocal(kTop - 1, 1).data(), local.data() + 8191);
  EXPECT_TRUE(mem_meld.ToLocal(kTop - 1, 2).empty());
  EXPECT_TRUE(mem_meld.ToLocal(kTop, 1).empty());
  EXPECT_TRUE(mem_meld.ToLocal((1ull << 47) - 1, 1).empty());
  EXPECT_TRUE(mem_meld.ToLocal(UINT64_MAX, 1).empty());
  EXPECT_EQ(mem_meld.ToRemote(local.data() + 8191), kTop - 1);
}

TEST_F(Fixture, TranslationFromOtherThreads) {
  Attach();
  struct Seen {
    uintptr_t remote;
    const char* local;
    Size size;
  };
  std::atomic<bool> stop = false;
  std::atomic<uint64_t> read = 0;
  Vec<Vec<Seen>> seen(4);
  Vec<std::thread> readers;
  for (size_t i = 0; i < seen.size(); ++i)
    readers.emplace_back([&, i] {
      std::mt19937_64 rng(i);
      uint64_t sum = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        Vec<TestMirror::Rec> recent;
        {
          std::lock_guard lock(mirror->mutex);
          auto& history = mirror->history;
          recent.assign(history.end() - std::min<size_t>(history.size(), 64), history.end());
        }
        for (int k = 0; k < 1000; ++k) {
          const TestMirror::Rec& r = recent[rng() % recent.size()];
          uintptr_t remote = r.remote + rng() % r.local.size();
          Span<char> local =
              mirror->mem_meld.ToLocal(remote, 1 + rng() % (r.remote + r.local.size() - remote));
          if (!local.empty()) {
            sum += local.front() + local.back();
            if (seen[i].size() < 100000) seen[i].push_back({remote, local.data(), local.size()});
          }
          const char* p = r.local.data() + rng() % r.local.size();
          sum += *p;
          if (uintptr_t back = mirror->mem_meld.ToRemote(p); back && seen[i].size() < 100000)
            seen[i].push_back({back, p, 1});
          sum += mirror->mem_meld.Mirror().front();
        }
      }
      read.fetch_add(sum, std::memory_order_relaxed);
    });
  EXPECT_EQ(target.Command("threads 2"), "ok");
  EXPECT_EQ(target.Command("remaps 4000"), "ok");
  EXPECT_EQ(target.Command("threads_stop"), "ok");
  stop.store(true, std::memory_order_relaxed);
  for (std::thread& reader : readers) reader.join();
  mirror->Detach();
  std::map<uint64_t, Vec<const TestMirror::Rec*>> by_skew;
  for (const TestMirror::Rec& r : mirror->history)
    by_skew[(uint64_t)r.local.data() - r.remote].push_back(&r);
  size_t results = 0, unknown = 0;
  for (const Vec<Seen>& results_of_one : seen)
    for (const Seen& t : results_of_one) {
      ++results;
      const Vec<const TestMirror::Rec*>& regions = by_skew[(uint64_t)t.local - t.remote];
      unknown += std::none_of(regions.begin(), regions.end(), [&](const TestMirror::Rec* r) {
        return r->remote <= t.remote && t.remote + t.size <= r->remote + r->local.size();
      });
    }
  EXPECT_GT(results, 1000u);
  EXPECT_EQ(unknown, 0u) << "of " << results << " translations";
}

TEST_F(Fixture, MirrorIsReadableFromOtherThreads) {
  Attach();
  std::atomic<bool> stop = false;
  std::atomic<uint64_t> faults = 0, reads = 0;
  Vec<std::thread> readers;
  for (int i = 0; i < 4; ++i)
    readers.emplace_back([&] {
      while (!stop.load(std::memory_order_relaxed)) {
        Span<char> local = mirror->mem_meld.Mirror();
        if (local.empty()) continue;
        char byte;
        iovec l{&byte, 1}, r{local.data() + local.size() - 1, 1};
        faults.fetch_add(process_vm_readv(getpid(), &l, 1, &r, 1, 0) != 1,
                         std::memory_order_relaxed);
        reads.fetch_add(1, std::memory_order_relaxed);
      }
    });
  for (int i = 0; i < 5000; ++i) target.CommandAddr("mmap 4096 rw");
  stop.store(true, std::memory_order_relaxed);
  for (std::thread& reader : readers) reader.join();
  EXPECT_GT(reads.load(), 1000u);
  EXPECT_EQ(faults.load(), 0u);
}

TEST_F(Fixture, Threads) {
  Attach();
  EXPECT_EQ(target.Command("threads 3"), "ok");
  uintptr_t addr = target.CommandAddr("mmap 65536 rw");
  EXPECT_EQ(target.Command(f("write {:#x} 65536 31", addr)), "ok");
  EXPECT_TRUE(MirrorMatchesRemote(addr, 65536));
  EXPECT_EQ(target.Command("threads_stop"), "ok");
}

TEST_F(Fixture, MainStackGrowth) {
  Attach();
  Str reply = target.Command("recurse 512");
  EXPECT_EQ(reply.substr(0, 3), "ok ");
  target.CommandAddr("sbrk 0");
  ExpectContentMatches();
}

TEST_F(Fixture, CmdlineAndEnvironStayReadable) {
  Str cmdline = ReadFile(f("/proc/{}/cmdline", target.pid));
  Attach();
  EXPECT_EQ(ReadFile(f("/proc/{}/cmdline", target.pid)), cmdline);
  EXPECT_FALSE(ReadFile(f("/proc/{}/environ", target.pid)).empty());
  EXPECT_EQ(target.Command("setarg renamed"), "ok");
  EXPECT_TRUE(ReadFile(f("/proc/{}/cmdline", target.pid)).starts_with("renamed")) << cmdline;
}

TEST_F(Fixture, FileMappingsAreShownOrConverted) {
  Attach();
  uintptr_t shown = target.CommandAddr("mmapfile /etc/hostname 4096 r");
  uintptr_t writable = target.CommandAddr("mmapfile /etc/hostname 4096 rw");
  EXPECT_FALSE(Mappings(target.pid, shown, shown + 1)[0].Memfd());
  EXPECT_TRUE(Mappings(target.pid, writable, writable + 1)[0].Memfd());
  EXPECT_TRUE(MirrorMatchesRemote(shown, 4096));
  EXPECT_TRUE(MirrorMatchesRemote(writable, 4096));
  EXPECT_EQ(target.Command(f("mprotect {:#x} 4096 rw", shown)), "ok");
  EXPECT_EQ(target.Command(f("write {:#x} 16 9", shown)), "ok");
  EXPECT_TRUE(Mappings(target.pid, shown, shown + 1)[0].Memfd());
  EXPECT_TRUE(MirrorMatchesRemote(shown, 4096));
}

TEST_F(Fixture, FileMappedOverAnotherFileShowsTheNewFile) {
  Str dir = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
  Str paths[] = {f("{}/pm-first-XXXXXX", dir), f("{}/pm-second-XXXXXX", dir)};
  for (Str& path : paths) {
    int fd = mkstemp(path.data());
    ASSERT_GE(fd, 0);
    Vec<char> bytes(4096, path.contains("first") ? 'a' : 'b');
    ASSERT_EQ(write(fd, bytes.data(), bytes.size()), 4096);
    close(fd);
  }
  Attach();
  uintptr_t addr = target.CommandAddr(f("mmapfile {} 4096 r", paths[0]));
  EXPECT_EQ(target.CommandAddr(f("mmapfile {} 4096 r {:#x}", paths[1], addr)), addr);
  EXPECT_TRUE(MirrorMatchesRemote(addr, 4096));
  EXPECT_EQ(LocalMapping(addr).path, paths[1]);
  for (Str& path : paths) unlink(path.c_str());
}

TEST_F(Fixture, ForkIsolation) {
  Attach();
  Str reply = target.Command("forktest");
  ASSERT_EQ(reply.substr(0, 3), "ok ") << reply;
  EXPECT_NE(reply.find("mutex=ok"), Str::npos) << reply;
  size_t p = reply.find("parent=");
  size_t c = reply.find("child=");
  ASSERT_NE(p, Str::npos);
  ASSERT_NE(c, Str::npos);
  EXPECT_NE(reply.substr(p, 7 + 32), reply.substr(c, 6 + 32)) << reply;
}

TEST_F(Fixture, ForkChildKeepsMemoryUnmappedDuringTheFork) {
  Attach();
  EXPECT_EQ(target.Command("forkunmap 300"), "ok");
}

TEST_F(Fixture, ForkThenExec) {
  Attach();
  EXPECT_EQ(target.Command("fork"), "ok");
  EXPECT_EQ(target.Command("forkexec"), "ok status=0");
  uintptr_t addr = target.CommandAddr("mmap 65536 rw");
  EXPECT_EQ(target.Command(f("write {:#x} 65536 41", addr)), "ok");
  EXPECT_TRUE(MirrorMatchesRemote(addr, 65536));
}

TEST_F(Fixture, RawForkGetsItsTidAndPrivateMemory) {
  Attach();
  EXPECT_EQ(target.Command("rawfork clone"), "ok status=0");
  EXPECT_EQ(target.Command("rawfork clone3"), "ok status=0");
  EXPECT_EQ(target.Command("fork"), "ok");
}

TEST_F(Fixture, UntracedCloneGetsPrivateMemory) {
  Attach();
  EXPECT_EQ(target.Command("rawfork untraced"), "ok status=0");
  EXPECT_EQ(target.Command("fork"), "ok");
}

TEST_F(Fixture, ForkedChildHasNoMemfdDescriptor) {
  Attach();
  ASSERT_TRUE(Descriptors(target.pid).contains(kMemfdDescriptor));
  EXPECT_EQ(target.Command(f("forkfd {}", kMemfdDescriptor)), "ok open=0");
  EXPECT_EQ(target.Command("forkfd 0"), "ok open=1");
}

TEST_F(Fixture, SharedFileMappingsAreShared) {
  Str path = f("{}/pm-shared-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
  int fd = mkstemp(path.data());
  ASSERT_GE(fd, 0);
  ASSERT_EQ(ftruncate(fd, 8192), 0);
  uintptr_t before = target.CommandAddr(f("mmapfileshared {} 8192", path));
  Attach();
  uintptr_t after = target.CommandAddr(f("mmapfileshared {} 8192", path));
  for (uintptr_t addr : {before, after}) {
    EXPECT_FALSE(Mappings(target.pid, addr, addr + 1)[0].Memfd());
    EXPECT_EQ(target.Command(f("write {:#x} 16 {}", addr, addr == before ? 5 : 6)), "ok");
    Span<char> local = mirror->Local(addr);
    ASSERT_GE(local.size(), 8192u);
    EXPECT_TRUE(MirrorMatchesRemote(addr, 8192));
    local[100 + (addr == after)] = 'M';
    char seen = 0;
    ASSERT_TRUE(ReadRemote(target.pid, addr + 100 + (addr == after), &seen, 1));
    EXPECT_EQ(seen, 'M');
  }
  char file[8192];
  ASSERT_EQ(pread(fd, file, sizeof file, 0), (ssize_t)sizeof file);
  EXPECT_TRUE(MirrorMatchesRemote(after, 8192));
  EXPECT_EQ(memcmp(file, mirror->Local(after).data(), sizeof file), 0);
  EXPECT_EQ(file[100], 'M');
  EXPECT_EQ(file[101], 'M');
  close(fd);
  unlink(path.c_str());
}

TEST_F(Fixture, SharedMemoryStaysShared) {
  Attach();
  uintptr_t addr = target.CommandAddr("mmapshared 4096");
  EXPECT_EQ(target.Command(f("forkshared {:#x}", addr)), "ok");
  EXPECT_FALSE(Covered(addr));
}

TEST_F(Fixture, VforkAndCloneVmChildrenExec) {
  Attach();
  EXPECT_EQ(target.Command("vfork"), "ok status=0");
  EXPECT_EQ(target.Command("clonevm"), "ok status=0");
  EXPECT_EQ(target.Command("fork"), "ok");
}

TEST_F(Fixture, ForkedChildForksAgain) {
  Attach();
  EXPECT_EQ(target.Command("forktwice"), "ok status=0");
  EXPECT_EQ(target.Command("fork"), "ok");
}

TEST_F(Fixture, ForkReturnsWhenTheChildDiesBeforeItRuns) {
  Attach();
  EXPECT_EQ(target.Command("forkkilled 268435456"), "ok");
  EXPECT_EQ(target.Command("fork"), "ok");
}

TEST_F(Fixture, WipeOnForkAndDontForkMemoryStaysAnonymous) {
  uintptr_t wipe = target.CommandAddr("mmap 4096 rw");
  uintptr_t keep = target.CommandAddr("mmap 4096 rw");
  EXPECT_EQ(target.Command(f("madvise {:#x} 4096 wipeonfork", wipe)), "ok");
  EXPECT_EQ(target.Command(f("madvise {:#x} 4096 dontfork", keep)), "ok");
  Attach();
  EXPECT_FALSE(Mappings(target.pid, wipe, wipe + 1)[0].Memfd());
  EXPECT_FALSE(Mappings(target.pid, keep, keep + 1)[0].Memfd());
  EXPECT_FALSE(Covered(wipe));
  EXPECT_FALSE(Covered(keep));
}

TEST_F(Fixture, MadviseWipeOnForkMakesMemoryPrivate) {
  Attach();
  uintptr_t addr = target.CommandAddr("mmap 16384 rw");
  EXPECT_EQ(target.Command(f("write {:#x} 16384 3", addr)), "ok");
  EXPECT_EQ(target.Command(f("madvise {:#x} 16384 wipeonfork", addr)), "ok");
  EXPECT_FALSE(Mappings(target.pid, addr, addr + 1)[0].Memfd());
  EXPECT_FALSE(Covered(addr));
  EXPECT_EQ(target.Command(f("verify {:#x} 16384 3", addr)), "ok");
}

TEST_F(Fixture, AttachWhileAThreadHoldsMallocsLock) {
  EXPECT_EQ(target.Command("threads 1"), "ok");
  target.Send("mallocstats 2000");
  usleep(200'000);
  Attach();
  EXPECT_EQ(target.ReadLine(), "ok");
  EXPECT_EQ(target.Command("threads_stop"), "ok");
  ExpectContentMatches();
}

TEST_F(Fixture, AttachKeepsErrno) {
  EXPECT_EQ(target.Command("errno 77"), "ok");
  Attach();
  EXPECT_EQ(target.Command("errno"), "ok 77");
}

TEST_F(Fixture, DetachedOnTargetExit) {
  Attach();
  target.Command("exit 3");
  EXPECT_TRUE(mirror->WaitDetached());
  EXPECT_FALSE(mirror->detached_ok) << mirror->detached_status;
}

TEST_F(Fixture, ExecContinuesTheSession) {
  Attach();
  Path path = Path::ExecutablePath().Parent() / "mem_meld_target";
  for (StrView exec : {"exec", "threadexec"}) {
    EXPECT_EQ(target.Command("threads 2"), "ok");
    int attachments = mirror->attachments;
    target.Send(f("{} {}", exec, path.c_str()));
    ASSERT_EQ(target.ReadLine().substr(0, 4), "pid ") << exec;
    ASSERT_TRUE(mirror->WaitAttachments(attachments + 1)) << mirror->detached_status;
    EXPECT_FALSE(mirror->detached);
    EXPECT_TRUE(AnyMemfd(target.pid));
    uintptr_t addr = target.CommandAddr("mmap 65536 rw");
    EXPECT_EQ(target.Command(f("write {:#x} 65536 7", addr)), "ok");
    EXPECT_TRUE(MirrorMatchesRemote(addr, 65536));
    ExpectRegionsMapped();
    ExpectContentMatches();
  }
  EXPECT_EQ(target.Command("fork"), "ok");
}

TEST_F(Fixture, DestroyLeavesAnOrdinaryProcess) {
  Attach();
  uintptr_t addr = target.CommandAddr("mmap 65536 rw");
  EXPECT_EQ(target.Command(f("write {:#x} 65536 44", addr)), "ok");
  EXPECT_EQ(target.Command("threads 2"), "ok");
  Destroy();
  EXPECT_EQ(TracerPid(target.pid), 0);
  EXPECT_FALSE(AnyMemfd(target.pid));
  EXPECT_FALSE(Descriptors(target.pid).contains(kMemfdDescriptor));
  EXPECT_EQ(target.Command(f("verify {:#x} 65536 44", addr)), "ok");
  EXPECT_EQ(target.Command("threads_stop"), "ok");
  EXPECT_EQ(target.Command("forktest").substr(0, 11), "ok mutex=ok");
  EXPECT_EQ(target.Command("forkexec"), "ok status=0");
}

TEST_F(Fixture, DetachFromAnotherThreadWhileTheTargetChurns) {
  Attach();
  uintptr_t addr = target.CommandAddr("mmap 65536 rw");
  EXPECT_EQ(target.Command(f("write {:#x} 65536 8", addr)), "ok");
  EXPECT_EQ(target.Command("threads 4"), "ok");
  target.Send("remaps 4000");
  usleep(50'000);
  std::thread([&] { mirror->mem_meld.Detach(); }).join();
  int events = mirror->Events();
  EXPECT_TRUE(mirror->detached_ok) << mirror->detached_status;
  EXPECT_EQ(TracerPid(target.pid), 0);
  EXPECT_FALSE(AnyMemfd(target.pid));
  EXPECT_FALSE(Descriptors(target.pid).contains(kMemfdDescriptor));
  EXPECT_EQ(target.ReadLine(), "ok");
  EXPECT_EQ(target.Command(f("verify {:#x} 65536 8", addr)), "ok");
  EXPECT_EQ(target.Command("threads_stop"), "ok");
  EXPECT_EQ(target.Command("forktest").substr(0, 11), "ok mutex=ok");
  EXPECT_EQ(mirror->Events(), events);
}

TEST_F(Fixture, DetachFromACallback) {
  int pidfd = (int)syscall(SYS_pidfd_open, target.pid, 0);
  ASSERT_GE(pidfd, 0);
  mirror = std::make_unique<TestMirror>();
  mirror->detach_on_attached = true;
  mirror->Attach(pidfd);
  close(pidfd);
  EXPECT_TRUE(mirror->WaitDetached());
  EXPECT_TRUE(mirror->detached_ok) << mirror->detached_status;
  EXPECT_EQ(TracerPid(target.pid), 0);
  EXPECT_FALSE(AnyMemfd(target.pid));
  EXPECT_EQ(target.Command("forktest").substr(0, 11), "ok mutex=ok");
}

TEST_F(Fixture, DetachBeforeAttachEndsTheSession) {
  int pidfd = (int)syscall(SYS_pidfd_open, target.pid, 0);
  ASSERT_GE(pidfd, 0);
  mirror = std::make_unique<TestMirror>();
  mirror->mem_meld.Detach();
  mirror->Attach(pidfd);
  close(pidfd);
  EXPECT_TRUE(mirror->WaitDetached());
  EXPECT_TRUE(mirror->detached_ok) << mirror->detached_status;
  EXPECT_EQ(TracerPid(target.pid), 0);
  EXPECT_FALSE(AnyMemfd(target.pid));
  EXPECT_EQ(target.Command("forktest").substr(0, 11), "ok mutex=ok");
}

TEST_F(Fixture, AttachRestoresTheSignalMaskOfItsThread) {
  int pidfd = (int)syscall(SYS_pidfd_open, target.pid, 0);
  ASSERT_GE(pidfd, 0);
  MemMeld bare;
  std::atomic<bool> attached = false;
  bare.on_attached = [&] { attached.store(true, std::memory_order_relaxed); };
  sigset_t before, after;
  std::thread thread([&] {
    sigfillset(&before);
    pthread_sigmask(SIG_SETMASK, &before, nullptr);
    pthread_sigmask(SIG_BLOCK, nullptr, &before);
    Status status;
    bare.Attach(pidfd, status);
    pthread_sigmask(SIG_BLOCK, nullptr, &after);
  });
  for (int i = 0; i < 2000 && !attached.load(std::memory_order_relaxed); ++i) usleep(10'000);
  EXPECT_TRUE(attached.load(std::memory_order_relaxed));
  bare.Detach();
  thread.join();
  close(pidfd);
  for (int signo = 1; signo < NSIG; ++signo)
    EXPECT_EQ(sigismember(&before, signo), sigismember(&after, signo)) << signo;
  EXPECT_EQ(TracerPid(target.pid), 0);
}

TEST_F(Fixture, GuardPageInstalledBeforeAttachStaysInTheTarget) {
  uintptr_t addr = target.CommandAddr("mmap 16384 rw");
  EXPECT_EQ(target.Command(f("write {:#x} 4096 5", addr)), "ok");
  EXPECT_EQ(target.Command(f("write {:#x} 8192 6", addr + 8192)), "ok");
  if (target.Command(f("madvise {:#x} 4096 guard", addr + 4096)) != "ok")
    GTEST_SKIP() << "the kernel has no guard regions";
  Attach();
  EXPECT_TRUE(Covered(addr));
  EXPECT_FALSE(Covered(addr + 4096));
  EXPECT_TRUE(Covered(addr + 8192));
  EXPECT_TRUE(MirrorMatchesRemote(addr, 4096));
  EXPECT_TRUE(MirrorMatchesRemote(addr + 8192, 8192));
  char byte;
  EXPECT_FALSE(ReadRemote(target.pid, addr + 4096, &byte, 1));
  Destroy();
  EXPECT_FALSE(ReadRemote(target.pid, addr + 4096, &byte, 1));
  EXPECT_EQ(target.Command(f("verify {:#x} 4096 5", addr)), "ok");
  EXPECT_EQ(target.Command(f("verify {:#x} 8192 6", addr + 8192)), "ok");
}

TEST_F(Fixture, GuardPageInstalledAfterAttachLeavesTheMirror) {
  Attach();
  uintptr_t addr = target.CommandAddr("mmap 16384 rw");
  EXPECT_EQ(target.Command(f("write {:#x} 4096 5", addr)), "ok");
  EXPECT_EQ(target.Command(f("write {:#x} 8192 6", addr + 8192)), "ok");
  if (target.Command(f("madvise {:#x} 4096 guard", addr + 4096)) != "ok")
    GTEST_SKIP() << "the kernel has no guard regions";
  EXPECT_TRUE(Covered(addr));
  EXPECT_FALSE(Covered(addr + 4096));
  EXPECT_TRUE(Covered(addr + 8192));
  EXPECT_TRUE(MirrorMatchesRemote(addr, 4096));
  EXPECT_TRUE(MirrorMatchesRemote(addr + 8192, 8192));
  char byte;
  EXPECT_FALSE(ReadRemote(target.pid, addr + 4096, &byte, 1));
  EXPECT_EQ(target.Command(f("madvise {:#x} 4096 unguard", addr + 4096)), "ok");
  EXPECT_TRUE(Covered(addr + 4096));
  EXPECT_TRUE(ReadRemote(target.pid, addr + 4096, &byte, 1));
  EXPECT_EQ(target.Command(f("madvise {:#x} 4096 guard", addr + 4096)), "ok");
  EXPECT_FALSE(Covered(addr + 4096));
  Destroy();
  EXPECT_FALSE(ReadRemote(target.pid, addr + 4096, &byte, 1));
  EXPECT_EQ(target.Command(f("verify {:#x} 4096 5", addr)), "ok");
  EXPECT_EQ(target.Command(f("verify {:#x} 8192 6", addr + 8192)), "ok");
  EXPECT_EQ(target.Command("fork"), "ok");
}

TEST_F(Fixture, ForkedChildIsFollowed) {
  Attach();
  uintptr_t addr = target.CommandAddr("mmap 65536 rw");
  EXPECT_EQ(target.Command(f("write {:#x} 65536 5", addr)), "ok");
  {
    std::lock_guard lock(mirror->mutex);
    mirror->follow = true;
  }
  Str reply = target.Command(f("forkhold {:#x} 4096 6", addr));
  ASSERT_EQ(reply.substr(0, 3), "ok ") << reply;
  pid_t child = atoi(reply.c_str() + 3);
  TestMirror* follower;
  {
    std::lock_guard lock(mirror->mutex);
    follower = mirror->children.empty() ? nullptr : mirror->children.back().get();
  }
  ASSERT_TRUE(follower);
  ASSERT_TRUE(follower->WaitAttached()) << follower->detached_status;
  EXPECT_EQ(TracerThreads(), 2);
  EXPECT_NE(TracerPid(child), TracerPid(target.pid));
  struct stat parent_memfd, child_memfd;
  ASSERT_EQ(stat(f("/proc/{}/fd/{}", target.pid, kMemfdDescriptor).c_str(), &parent_memfd), 0);
  ASSERT_EQ(stat(f("/proc/{}/fd/{}", child, kMemfdDescriptor).c_str(), &child_memfd), 0);
  EXPECT_NE(parent_memfd.st_ino, child_memfd.st_ino);
  EXPECT_TRUE(Mappings(child, addr, addr + 1)[0].Memfd());
  Span<char> local = follower->Local(addr);
  ASSERT_GE(local.size(), 65536u);
  Vec<char> bytes(65536);
  ASSERT_TRUE(ReadRemote(child, addr, bytes.data(), bytes.size()));
  EXPECT_EQ(memcmp(bytes.data(), local.data(), bytes.size()), 0);
  local[0] ^= 1;
  char seen = 0;
  ASSERT_TRUE(ReadRemote(child, addr, &seen, 1));
  EXPECT_EQ(seen, local[0]);
  EXPECT_EQ(target.Command(f("verify {:#x} 65536 5", addr)), "ok");
  EXPECT_EQ(target.Command("forkrelease"), "ok status=0");
  EXPECT_TRUE(follower->WaitDetached());
  EXPECT_TRUE(follower->detached_ok) << follower->detached_status;
  EXPECT_EQ(target.Command("forktest").substr(0, 11), "ok mutex=ok");
  EXPECT_EQ(target.Command("rawfork clone3"), "ok status=0");
}

TEST_F(Fixture, ForkChildWithSharedDescriptorsGetsNoOnForked) {
  Attach();
  {
    std::lock_guard lock(mirror->mutex);
    mirror->follow = true;
  }
  EXPECT_EQ(target.Command("rawfork clonefiles"), "ok status=0");
  {
    std::lock_guard lock(mirror->mutex);
    EXPECT_TRUE(mirror->children.empty());
  }
  EXPECT_TRUE(Descriptors(target.pid).contains(kMemfdDescriptor));
  uintptr_t addr = target.CommandAddr("mmap 65536 rw");
  EXPECT_EQ(target.Command(f("write {:#x} 65536 3", addr)), "ok");
  EXPECT_TRUE(MirrorMatchesRemote(addr, 65536));
}

TEST_F(Fixture, MirrorsTraceFromTheirOwnThreads) {
  Attach();
  Target second;
  ASSERT_NO_FATAL_FAILURE(second.Spawn());
  int pidfd = (int)syscall(SYS_pidfd_open, second.pid, 0);
  auto other = std::make_unique<TestMirror>();
  other->Attach(pidfd);
  close(pidfd);
  ASSERT_TRUE(other->WaitAttached()) << other->detached_status;
  EXPECT_EQ(TracerThreads(), 2);
  EXPECT_NE(TracerPid(second.pid), TracerPid(target.pid));
  uintptr_t addr = second.CommandAddr("mmap 65536 rw");
  EXPECT_EQ(second.Command(f("write {:#x} 65536 3", addr)), "ok");
  Destroy();
  EXPECT_EQ(TracerThreads(), 1);
  Span<char> local = other->Local(addr);
  Vec<char> bytes(65536);
  ASSERT_TRUE(ReadRemote(second.pid, addr, bytes.data(), bytes.size()));
  ASSERT_GE(local.size(), bytes.size());
  EXPECT_EQ(memcmp(bytes.data(), local.data(), bytes.size()), 0);
  other->Detach();
  other.reset();
  EXPECT_EQ(TracerThreads(), 0);
  second.Close();
  kill(second.pid, SIGKILL);
  waitpid(second.pid, nullptr, 0);
}

TEST_F(Fixture, SignalsArriveIntact) {
  Attach();
  EXPECT_EQ(target.Command(f("sigwatch {}", getpid())), "ok");
  EXPECT_EQ(target.Command("threads 3"), "ok");
  for (int i = 1; i <= 200; ++i) {
    sigval value = {.sival_int = i};
    sigqueue(target.pid, SIGRTMIN + 1, value);
    kill(target.pid, SIGUSR1);
  }
  Str count;
  for (int i = 0; i < 500 && !count.starts_with("ok rt=200 "); ++i) {
    usleep(10'000);
    count = target.Command("sigcount");
  }
  EXPECT_TRUE(count.starts_with("ok rt=200 values=20100 usr1=")) << count;
  EXPECT_TRUE(count.ends_with(" foreign=0")) << count;
  EXPECT_EQ(target.Command("threads_stop"), "ok");
}

TEST_F(Fixture, GroupStopsHoldEveryThread) {
  Attach();
  EXPECT_EQ(target.Command("threads 2"), "ok");
  kill(target.pid, SIGSTOP);
  usleep(200'000);
  target.Send("ping");
  pollfd readable{target.from, POLLIN, 0};
  EXPECT_EQ(poll(&readable, 1, 300), 0);
  kill(target.pid, SIGCONT);
  EXPECT_EQ(target.ReadLine(), "ok");
  EXPECT_EQ(target.Command("threads_stop"), "ok");
  EXPECT_FALSE(mirror->detached);
}

TEST_F(Fixture, ExitStatusReachesTheParent) {
  Attach();
  EXPECT_EQ(target.Command("threads 2"), "ok");
  target.Send("threadexit 7");
  int status = Reap();
  EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 7) << status;
  EXPECT_TRUE(mirror->WaitDetached());
  EXPECT_NE(mirror->detached_status.find("status 7"), Str::npos) << mirror->detached_status;
}

TEST_F(Fixture, DeathBySignalReachesTheParent) {
  Attach();
  EXPECT_EQ(target.Command("threads 2"), "ok");
  kill(target.pid, SIGTERM);
  int status = Reap();
  EXPECT_TRUE(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM) << status;
  EXPECT_TRUE(mirror->WaitDetached());
  EXPECT_NE(mirror->detached_status.find("signal 15"), Str::npos) << mirror->detached_status;
}

TEST_F(Fixture, ConcurrentUnmapAndMapConverge) {
  Attach();
  Str reply = target.Command("racemaps 2000");
  ASSERT_EQ(reply.substr(0, 3), "ok ") << reply;
  uintptr_t addr = strtoull(reply.c_str() + 3, nullptr, 16);
  bool mapped = !Mappings(target.pid, addr, addr + 65536).empty();
  EXPECT_EQ(Covered(addr), mapped);
  if (mapped) EXPECT_TRUE(MirrorMatchesRemote(addr, 65536));
  ExpectRegionsMapped();
  ExpectContentMatches();
  EXPECT_LT(MemfdBytes(target.pid), 32ull << 20);
}

TEST_F(Fixture, ConcurrentMapsNeverShareMemory) {
  Attach();
  EXPECT_EQ(target.Command("mapstamps 6 3000"), "ok");
}

TEST_F(Fixture, ClosingTheMemfdDescriptorDetaches) {
  Attach();
  uintptr_t addr = target.CommandAddr("mmap 4096 rw");
  EXPECT_EQ(target.Command(f("write {:#x} 4096 45", addr)), "ok");
  EXPECT_EQ(target.Command(f("close {}", kMemfdDescriptor)), "ok");
  uintptr_t after = target.CommandAddr("mmap 4096 rw");
  EXPECT_TRUE(mirror->WaitDetached());
  EXPECT_FALSE(mirror->detached_ok);
  EXPECT_NE(mirror->detached_status.find("memfd"), Str::npos) << mirror->detached_status;
  EXPECT_FALSE(AnyMemfd(target.pid));
  EXPECT_EQ(target.Command(f("verify {:#x} 4096 45", addr)), "ok");
  EXPECT_EQ(target.Command(f("write {:#x} 4096 46", after)), "ok");
  EXPECT_EQ(target.Command("fork"), "ok");
}

TEST_F(Fixture, RefusesASeccompTarget) {
  EXPECT_EQ(target.Command("seccomp"), "ok");
  ASSERT_NO_FATAL_FAILURE(StartAttach());
  EXPECT_FALSE(mirror->WaitAttached());
  EXPECT_NE(mirror->detached_status.find("seccomp"), Str::npos) << mirror->detached_status;
  EXPECT_EQ(TracerPid(target.pid), 0);
  EXPECT_EQ(target.Command("fork"), "ok");
}

TEST_F(Fixture, InstallingASeccompFilterDetaches) {
  Attach();
  EXPECT_EQ(target.Command("seccomp"), "ok");
  EXPECT_TRUE(mirror->WaitDetached());
  EXPECT_NE(mirror->detached_status.find("seccomp"), Str::npos) << mirror->detached_status;
  EXPECT_FALSE(AnyMemfd(target.pid));
  EXPECT_EQ(target.Command("fork"), "ok");
}

TEST_F(Fixture, RefusesATracedTarget) {
  Attach();
  std::unique_ptr<TestMirror> first = std::move(mirror);
  ASSERT_NO_FATAL_FAILURE(StartAttach());
  EXPECT_FALSE(mirror->WaitAttached());
  uintptr_t addr = target.CommandAddr("mmap 4096 rw");
  {
    std::lock_guard lock(first->mutex);
    EXPECT_TRUE(first->live.contains(addr));
  }
  first->Detach();
}

TEST_F(Fixture, AFailedAttachLeavesTheTargetAttachable) {
  std::set<int> fds = Descriptors(target.pid);
  rlimit limit;
  ASSERT_EQ(prlimit(target.pid, RLIMIT_NOFILE, nullptr, &limit), 0);
  rlim_t lowest_free = 0;
  while (fds.contains((int)lowest_free)) ++lowest_free;
  rlimit full = {lowest_free, limit.rlim_max};
  ASSERT_EQ(prlimit(target.pid, RLIMIT_NOFILE, &full, nullptr), 0);
  Str maps = ReadFile(f("/proc/{}/maps", target.pid));
  ASSERT_NO_FATAL_FAILURE(StartAttach());
  EXPECT_FALSE(mirror->WaitAttached());
  EXPECT_EQ(Descriptors(target.pid), fds);
  EXPECT_EQ(ReadFile(f("/proc/{}/maps", target.pid)), maps);
  EXPECT_EQ(TracerPid(target.pid), 0);
  Destroy();
  ASSERT_EQ(prlimit(target.pid, RLIMIT_NOFILE, &limit, nullptr), 0);
  Attach();
  EXPECT_EQ(target.Command("fork"), "ok");
}

}  // namespace
