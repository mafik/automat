// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "mem_meld.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>

#include "fd.hpp"
#include "format.hpp"
#include "linux_syscall.hpp"
#include "vec.hpp"

namespace automat {

namespace {

constexpr uint64_t kPage = 4096;
constexpr uint64_t kMirrorReserve = 256ull << 30;
constexpr uint64_t kUserEnd = 1ull << 47;
constexpr uint64_t kSlot = 1ull << 36;
constexpr uint64_t kMemfdSize = 1ull << 60;
constexpr uint64_t kNoOffset = ~0ull;
constexpr uint64_t kFileKey = 1ull << 63;
constexpr int kIndexBits = 28;
constexpr uint64_t kIndexMask = (1ull << kIndexBits) - 1;
constexpr uint64_t kPageMask = kUserEnd / kPage - 1;
constexpr long kRemoteFd = 900;
constexpr uint64_t kStackGuard = 256 * kPage;
constexpr uint64_t kPagemapGuardRegion = 1ull << 58;
constexpr int kMadvGuardInstall = 102;
constexpr int kMadvGuardRemove = 103;
constexpr int kMapGrowsDown = 0x100;
constexpr int kMremapDontUnmap = 4;
constexpr int kSeekData = 3;
constexpr int kSeekHole = 4;
constexpr int kFallocKeepSize = 1;
constexpr int kFallocPunchHole = 2;
constexpr int kWaitAll = 0;
constexpr int kWaitPid = 1;
constexpr int kWaitNoThread = 0x20000000;
constexpr uint64_t kCloneVm = 0x100;
constexpr uint64_t kCloneChildClearTid = 0x200000;
constexpr uint64_t kCloneUntraced = 0x800000;
constexpr uint64_t kCloneChildSetTid = 0x1000000;
constexpr int kPtraceSyscall = 24;
constexpr int kPtraceDetach = 17;
constexpr int kPtraceGetRegs = 12;
constexpr int kPtraceSetRegs = 13;
constexpr int kPtraceGetEventMsg = 0x4201;
constexpr int kPtraceSeize = 0x4206;
constexpr int kPtraceInterrupt = 0x4207;
constexpr int kPtraceListen = 0x4208;
constexpr int kPtraceGetSigmask = 0x420a;
constexpr int kPtraceSetSigmask = 0x420b;
constexpr int kPtraceGetSyscallInfo = 0x420e;
constexpr int kSyscallInfoEntry = 1;
constexpr int kSyscallInfoExit = 2;
constexpr int kEventFork = 1;
constexpr int kEventClone = 3;
constexpr int kEventExec = 4;
constexpr int kEventExit = 6;
constexpr int kEventStop = 128;
constexpr int kTraceSysGood = 1;
constexpr int kTraceFork = 1 << kEventFork;
constexpr int kTraceClone = 1 << kEventClone;
constexpr int kTraceExec = 1 << kEventExec;
constexpr int kTraceExit = 1 << kEventExit;
constexpr uint32_t kAuditArchX86_64 = 0xc000003e;
constexpr int kKcmpFile = 0;
constexpr int kKcmpVm = 1;
constexpr int kKcmpFiles = 2;
constexpr int kSeccompSetModeStrict = 0;
constexpr int kSeccompSetModeFilter = 1;
constexpr uint64_t kPageIsPresent = 1 << 3;
constexpr uint64_t kPageIsSwapped = 1 << 4;
constexpr uint64_t kPageIsPfnZero = 1 << 5;
constexpr uint64_t kProcmapVmaReadable = 0x01;
constexpr uint64_t kProcmapVmaWritable = 0x02;
constexpr uint64_t kProcmapVmaExecutable = 0x04;
constexpr uint64_t kProcmapVmaShared = 0x08;
constexpr uint64_t kProcmapQueryCoveringOrNextVma = 0x10;

struct PtraceSyscallInfo {
  uint8_t op;
  uint8_t pad[3];
  uint32_t arch;
  uint64_t instruction_pointer;
  uint64_t stack_pointer;
  union {
    struct {
      uint64_t nr;
      uint64_t args[6];
    } entry;
    struct {
      int64_t rval;
      uint8_t is_error;
    } exit;
  };
};

struct PageRegion {
  uint64_t start, end, categories;
};

struct PmScanArg {
  uint64_t size, flags, start, end, walk_end, vec, vec_len, max_pages, category_inverted,
      category_mask, category_anyof_mask, return_mask;
};

struct ProcmapQuery {
  uint64_t size, query_flags, query_addr, vma_start, vma_end, vma_flags, vma_page_size, vma_offset,
      inode;
  uint32_t dev_major, dev_minor, vma_name_size, build_id_size;
  uint64_t vma_name_addr, build_id_addr;
};

constexpr unsigned long IocReadWrite(unsigned long number, unsigned long size) {
  return 3ul << 30 | size << 16 | 'f' << 8 | number;
}
constexpr unsigned long kPagemapScan = IocReadWrite(16, sizeof(PmScanArg));
constexpr unsigned long kProcmapQuery = IocReadWrite(17, sizeof(ProcmapQuery));
constexpr long kOptions = kTraceSysGood | kTraceClone | kTraceFork | kTraceExec | kTraceExit;
constexpr int kSyscallStop = SIGTRAP | 0x80;
constexpr int kInterruptStop = SIGTRAP | kEventStop << 8;
constexpr long kErestartSys = 512, kErestartRestartBlock = 516;

uint64_t PageDown(uint64_t x) { return x & ~(kPage - 1); }
uint64_t PageUp(uint64_t x) { return PageDown(x + kPage - 1); }
uint64_t PackTranslation(uint64_t from, uint64_t to, uint32_t idx) {
  return ((to / kPage - from / kPage) & kPageMask) << kIndexBits | idx;
}
uint64_t Translate(uint64_t address, uint64_t value) {
  return ((address / kPage + (value >> kIndexBits)) & kPageMask) * kPage + address % kPage;
}
bool IsError(long result) { return (unsigned long)result > -4096ul; }

Str ReadWholeFile(const Str& path) {
  int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return {};
  Str out;
  char buf[65536];
  for (ssize_t n; (n = read(fd, buf, sizeof(buf))) > 0;) out.append(buf, n);
  close(fd);
  return out;
}

struct MapEntry {
  uint64_t start = 0, end = 0;
  int prot = 0;
  bool shared = false;
  uint64_t file_offset = 0;
  dev_t dev = 0;
  uint64_t inode = 0;
  uint64_t anonymous = 0;
  StrView path;
  StrView vm_flags;
  bool HasFlag(StrView token) const {
    for (size_t i = 0; i + token.size() <= vm_flags.size(); ++i)
      if (vm_flags.substr(i, token.size()) == token && (i == 0 || vm_flags[i - 1] == ' ') &&
          (i + token.size() == vm_flags.size() || vm_flags[i + token.size()] == ' '))
        return true;
    return false;
  }
};

Vec<std::pair<uint64_t, uint64_t>> GuardRuns(int pagemap_fd, uint64_t start, uint64_t end) {
  Vec<std::pair<uint64_t, uint64_t>> out;
  PageRegion vec[128];
  uint64_t entries[512];
  for (uint64_t cur = start; cur < end;) {
    PmScanArg arg = {};
    arg.size = sizeof(arg);
    arg.start = cur;
    arg.end = end;
    arg.vec = (uint64_t)vec;
    arg.vec_len = std::size(vec);
    arg.category_anyof_mask = kPageIsSwapped;
    arg.return_mask = kPageIsSwapped;
    long n = ioctl(pagemap_fd, kPagemapScan, &arg);
    if (n < 0) break;
    for (long i = 0; i < n; ++i) {
      for (uint64_t at = vec[i].start; at < vec[i].end;) {
        uint64_t count = std::min<uint64_t>(512, (vec[i].end - at) / kPage);
        if (pread(pagemap_fd, entries, count * 8, at / kPage * 8) != (ssize_t)(count * 8)) break;
        for (uint64_t k = 0; k < count; ++k) {
          if (!(entries[k] & kPagemapGuardRegion)) continue;
          uint64_t page = at + k * kPage;
          if (!out.empty() && out.back().second == page)
            out.back().second += kPage;
          else
            out.push_back({page, page + kPage});
        }
        at += count * kPage;
      }
    }
    if (arg.walk_end <= cur) break;
    cur = arg.walk_end;
  }
  return out;
}

void CopyIntoMirror(int mirror_local_fd, uint64_t memfd_offset, int procmem, int pagemap,
                    uint64_t remote, uint64_t len, bool whole) {
  void* local =
      mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, mirror_local_fd, memfd_offset);
  if (local == MAP_FAILED) return;
  Vec<std::pair<uint64_t, uint64_t>> runs;
  if (whole) {
    runs.push_back({remote, remote + len});
  } else {
    Vec<PageRegion> vec(4096);
    for (uint64_t cur = remote; cur < remote + len;) {
      PmScanArg arg = {};
      arg.size = sizeof(arg);
      arg.start = cur;
      arg.end = remote + len;
      arg.vec = (uint64_t)vec.data();
      arg.vec_len = vec.size();
      arg.category_anyof_mask = kPageIsPresent | kPageIsSwapped;
      arg.return_mask = kPageIsPresent | kPageIsSwapped | kPageIsPfnZero;
      long n = ioctl(pagemap, kPagemapScan, &arg);
      if (n < 0) break;
      for (long i = 0; i < n; ++i) {
        if ((vec[i].categories & kPageIsPfnZero) && !(vec[i].categories & kPageIsSwapped)) continue;
        if (!runs.empty() && runs.back().second == vec[i].start)
          runs.back().second = vec[i].end;
        else
          runs.push_back({vec[i].start, vec[i].end});
      }
      if (arg.walk_end <= cur) break;
      cur = arg.walk_end;
    }
  }
  for (auto [a, b] : runs)
    for (uint64_t p = a; p < b;) {
      ssize_t n = pread(procmem, (char*)local + (p - remote), b - p, p);
      p += n > 0 ? n : kPage;
    }
  munmap(local, len);
}

template <typename Fn>
void ForEachExtent(int fd, uint64_t offset, uint64_t len, Fn&& fn) {
  for (uint64_t at = offset, end = offset + len; at < end;) {
    off_t data = lseek(fd, at, kSeekData);
    if (data < 0 || (uint64_t)data >= end) return;
    off_t hole = lseek(fd, data, kSeekHole);
    uint64_t stop = hole < 0 ? end : std::min<uint64_t>(hole, end);
    fn((uint64_t)data, stop - data);
    at = stop;
  }
}

struct Mapping {
  uint64_t remote, len;
  int prot;
  uint64_t key;
  FD file;
  Str path;
  uint64_t file_offset;
};

Status ErrorStatus(StrView message, SourceLocation location = std::source_location::current()) {
  errno = 0;
  Status status;
  AppendErrorMessage(status, location) += message;
  return status;
}

int WakeSignal() { return SIGRTMIN + 4; }

struct Vma {
  uint64_t start = 0, end = 0;
  uint64_t offset = 0;
  int prot = 0;
  bool shared = false;
  dev_t dev = 0;
  uint64_t inode = 0;
};

bool QueryVma(int maps, uint64_t addr, Vma& v, Str* name = nullptr) {
  char path[4096];
  ProcmapQuery q = {.size = sizeof q,
                    .query_flags = kProcmapQueryCoveringOrNextVma,
                    .query_addr = addr,
                    .vma_name_size = name ? (uint32_t)sizeof path : 0,
                    .vma_name_addr = name ? (uint64_t)path : 0};
  if (ioctl(maps, kProcmapQuery, &q) != 0) return false;
  v = {q.vma_start,
       q.vma_end,
       q.vma_offset,
       (q.vma_flags & kProcmapVmaReadable ? PROT_READ : 0) |
           (q.vma_flags & kProcmapVmaWritable ? PROT_WRITE : 0) |
           (q.vma_flags & kProcmapVmaExecutable ? PROT_EXEC : 0),
       (q.vma_flags & kProcmapVmaShared) != 0,
       makedev(q.dev_major, q.dev_minor),
       q.inode};
  if (name) *name = q.vma_name_size ? Str(path) : Str();
  return true;
}

Vec<Vma> VmasIn(int maps, uint64_t lo, uint64_t hi) {
  Vec<Vma> out;
  for (Vma v; lo < hi && QueryVma(maps, lo, v) && v.start < hi; lo = v.end) {
    uint64_t skip = v.start < lo ? lo - v.start : 0;
    out.push_back(v);
    out.back().start += skip;
    out.back().offset += skip;
    out.back().end = std::min(v.end, hi);
  }
  return out;
}

Vec<std::pair<uint64_t, uint64_t>> Cut(std::map<uint64_t, uint64_t>& set, uint64_t a, uint64_t b) {
  Vec<std::pair<uint64_t, uint64_t>> cut;
  auto it = set.upper_bound(a);
  if (it != set.begin() && std::prev(it)->second > a) --it;
  while (it != set.end() && it->first < b) {
    auto [start, end] = *it;
    it = set.erase(it);
    if (start < a) set.emplace(start, a);
    if (end > b) set.emplace(b, end);
    cut.push_back({std::max(start, a), std::min(end, b)});
  }
  return cut;
}

struct Slots {
  struct Slot {
    std::map<uint64_t, uint64_t> used;
    uint64_t epoch = 0;
    bool file = false;
    Str path;
    uint64_t file_offset = 0;
  };
  int memfd = -1;
  Vec<Slot> slots;
  Vec<uint64_t> free;
  uint64_t epochs = 0;

  void Use(uint64_t offset, uint64_t len) {
    for (uint64_t at = offset, end; at < offset + len; at = end) {
      uint64_t slot = at / kSlot, base = slot * kSlot;
      end = std::min(offset + len, base + kSlot);
      if (slots.size() <= slot) slots.resize(slot + 1);
      Cut(slots[slot].used, at - base, end - base);
      slots[slot].used.emplace(at - base, end - base);
    }
  }

  uint64_t Take(uint64_t len, uint64_t room, bool file = false, StrView path = {},
                uint64_t file_offset = 0) {
    uint64_t slot = room <= kSlot && !free.empty() ? free.back() : slots.size();
    if (slot * kSlot + room > kMemfdSize) return kNoOffset;
    if (slot == slots.size())
      slots.resize(slot + (room + kSlot - 1) / kSlot);
    else
      free.pop_back();
    Use(slot * kSlot, len);
    slots[slot].file = file;
    slots[slot].path = Str(path);
    slots[slot].file_offset = file_offset;
    return slot * kSlot;
  }

  const Slot* At(uint64_t offset) const {
    return offset / kSlot < slots.size() ? &slots[offset / kSlot] : nullptr;
  }
  bool File(uint64_t offset) const { return At(offset) && At(offset)->file; }
  StrView Path(uint64_t offset) const { return At(offset) ? StrView(At(offset)->path) : ""; }
  uint64_t FileOffset(uint64_t offset) const {
    return File(offset) ? At(offset)->file_offset + offset % kSlot : 0;
  }
  uint64_t Epoch(uint64_t offset) const { return At(offset) ? At(offset)->epoch : 0; }

  bool CanExtend(uint64_t end, uint64_t len) const {
    const Slot* s = At(end - 1);
    uint64_t base = (end - 1) / kSlot * kSlot;
    return s && !s->used.empty() && s->used.rbegin()->second == end - base &&
           end + len <= base + kSlot;
  }

  void Release(uint64_t offset, uint64_t len) {
    for (uint64_t at = offset, end; at < offset + len; at = end) {
      uint64_t slot = at / kSlot, base = slot * kSlot;
      end = std::min(offset + len, base + kSlot);
      if (slot >= slots.size()) return;
      Slot& s = slots[slot];
      Vec<std::pair<uint64_t, uint64_t>> cut = Cut(s.used, at - base, end - base);
      for (auto [a, b] : cut)
        LinuxSyscall(__NR_fallocate, memfd, kFallocKeepSize | kFallocPunchHole, base + a, b - a);
      if (cut.empty()) continue;
      s.epoch = ++epochs;
      if (s.used.empty()) {
        s = {.epoch = s.epoch};
        free.push_back(slot);
      }
    }
  }
};

struct Piece {
  uint64_t remote, len, key;
  uint64_t epoch = 0;

  Piece Part(uint64_t a, uint64_t b) const { return {a, b - a, key + (a - remote), epoch}; }
};

struct Call {
  long nr = -1;
  uint64_t args[6] = {};
  uint64_t offset = kNoOffset;
  uint64_t epoch = 0;
  Vec<Piece> pieces;
  Vma source;
  long result = 0;
  uint64_t clone_flags = 0;
  uint64_t child_tid = 0;
  uint64_t mremaps = 0;
  bool moving = false;
  bool counted = false;
};

Vec<Piece> PiecesWithout(const Vec<Piece>& pieces, uint64_t key, uint64_t len) {
  Vec<Piece> out;
  for (const Piece& p : pieces) {
    uint64_t a = p.key, b = p.key + p.len;
    if (b <= key || a >= key + len) {
      out.push_back(p);
      continue;
    }
    if (a < key) out.push_back(p.Part(p.remote, p.remote + (key - a)));
    if (b > key + len) out.push_back(p.Part(p.remote + (key + len - a), p.remote + p.len));
  }
  return out;
}

uint64_t Stripped(uint64_t clone_flags) {
  return kCloneUntraced | (clone_flags & kCloneVm ? 0 : kCloneChildSetTid | kCloneChildClearTid);
}

int WaitStop(pid_t tid, int options = 0) {
  for (;;) {
    siginfo_t info = {};
    long result = LinuxSyscall(__NR_waitid, kWaitPid, tid, &info,
                               WSTOPPED | __WALL | kWaitNoThread | options, nullptr);
    if (result == 0) return info.si_pid ? info.si_status : 0;
    if (result != -EINTR) return -1;
  }
}

struct Task {
  pid_t tid = 0;
  bool known = false;
  bool running = true;
  int stop = 0;
  int signal = 0;
  Call call;
  user_regs_struct saved;
  uint64_t mask = 0;
  bool injected = false;
  bool rewind = false;

  bool Injectable() const { return known && stop >> 8 != kEventExit; }

  bool NoteExitStop(int reported) {
    if (reported >> 8 != kEventExit) return false;
    stop = reported;
    injected = rewind = false;
    return true;
  }

  bool AtExitStop() { return stop >> 8 == kEventExit || NoteExitStop(WaitStop(tid, WNOHANG)); }

  int ResumeAndWait(int pass) {
    if (AtExitStop() || LinuxSyscall(__NR_ptrace, kPtraceSyscall, tid, 0, pass) != 0) return -1;
    int reported = WaitStop(tid);
    return NoteExitStop(reported) ? -1 : reported;
  }

  void Restore() {
    if (!injected) return;
    if (rewind) {
      saved.rip -= 2;
      saved.rax = saved.orig_rax;
    }
    LinuxSyscall(__NR_ptrace, kPtraceSetRegs, tid, 0, &saved);
    LinuxSyscall(__NR_ptrace, kPtraceSetSigmask, tid, sizeof mask, &mask);
    injected = rewind = false;
    long error = -(long)saved.rax;
    bool restart =
        (long)saved.orig_rax >= 0 && error >= kErestartSys && error <= kErestartRestartBlock;
    bool group = stop >> 8 == kEventStop && (stop & 0xff) != SIGTRAP;
    stop = kSyscallStop;
    if (group || restart) {
      LinuxSyscall(__NR_ptrace, kPtraceInterrupt, tid, 0, 0);
      stop = kInterruptStop;
    }
  }

  void SetRegs(const std::function<void(user_regs_struct&)>& change) {
    user_regs_struct r;
    if (LinuxSyscall(__NR_ptrace, kPtraceGetRegs, tid, 0, &r) != 0) return;
    change(r);
    LinuxSyscall(__NR_ptrace, kPtraceSetRegs, tid, 0, &r);
  }

  void Cancel(int error) {
    SetRegs([](user_regs_struct& r) { r.orig_rax = -1; });
    call.result = -error;
  }
};

}  // namespace

uint64_t MemMeld::RegionTable::Rec::End() const { return r.remote + r.local.size(); }

uint64_t MemMeld::RegionTable::Rec::Delta() const { return key - r.remote; }

MemMeld::RegionTable::RegionTable()
    : mirror([] {
        void* reserved = mmap(nullptr, kMirrorReserve, PROT_NONE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        return reserved == MAP_FAILED ? nullptr : (char*)reserved;
      }()) {
  pool.emplace_back();
}

MemMeld::RegionTable::~RegionTable() {
  if (mirror) munmap(mirror, kMirrorReserve);
}

uint64_t MemMeld::RegionTable::AllocLocal(uint64_t len) {
  for (size_t i = 0; i < free_list.size(); ++i) {
    if (free_list[i].second >= len) {
      uint64_t off = free_list[i].first;
      if (free_list[i].second == len)
        free_list.erase(free_list.begin() + i);
      else {
        free_list[i].first += len;
        free_list[i].second -= len;
      }
      return off;
    }
  }
  uint64_t off = high_water.load(std::memory_order_relaxed);
  return off + len > kMirrorReserve ? kNoOffset : off;
}

void MemMeld::RegionTable::FreeLocal(uint64_t off, uint64_t len) {
  mmap(mirror + off, len, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1,
       0);
  if (off + len >= high_water.load(std::memory_order_relaxed))
    high_water.store(off, std::memory_order_relaxed);
  else
    free_list.push_back({off, len});
}

uint32_t MemMeld::RegionTable::AddRec(uint64_t remote, uint64_t len, int prot, uint64_t key,
                                      StrView path, uint64_t file_offset, char* local, dev_t dev,
                                      uint64_t inode) {
  uint32_t idx;
  if (free_pool.empty()) {
    idx = pool.size();
    pool.emplace_back();
  } else {
    idx = free_pool.back();
    free_pool.pop_back();
  }
  Rec& rec = pool[idx];
  rec.path = Str(path);
  rec.key = key;
  rec.dev = dev;
  rec.inode = inode;
  rec.r = {remote, Span<char>(local, len), prot, rec.path, file_offset};
  by_remote.Update(remote, remote + len, PackTranslation(remote, (uint64_t)local, idx));
  by_local.Update((uint64_t)local, (uint64_t)local + len,
                  PackTranslation((uint64_t)local, remote, idx));
  return idx;
}

void MemMeld::RegionTable::DropRec(uint32_t idx) {
  Rec& rec = pool[idx];
  by_remote.Erase(rec.r.remote, rec.End());
  by_local.Erase((uint64_t)rec.r.local.data(), (uint64_t)rec.r.local.data() + rec.r.local.size());
  rec.path.clear();
  free_pool.push_back(idx);
}

uint32_t MemMeld::RegionTable::MapRegion(uint64_t remote, uint64_t len, int prot, uint64_t key,
                                         int file, StrView path, uint64_t file_offset, int memfd) {
  uint64_t off = AllocLocal(len);
  if (off == kNoOffset) return 0;
  struct stat st = {};
  if (file >= 0) fstat(file, &st);
  bool write = file < 0 || (fcntl(file, F_GETFL) & O_ACCMODE) == O_RDWR;
  void* local =
      mmap(mirror + off, len, write ? PROT_READ | PROT_WRITE : PROT_READ, MAP_SHARED | MAP_FIXED,
           file >= 0 ? file : memfd, file >= 0 ? file_offset : key);
  if (local == MAP_FAILED) {
    FreeLocal(off, len);
    return 0;
  }
  if (off + len > high_water.load(std::memory_order_relaxed))
    high_water.store(off + len, std::memory_order_relaxed);
  return AddRec(remote, len, prot, key, path, file_offset, mirror + off, st.st_dev, st.st_ino);
}

void MemMeld::RegionTable::UnmapRegion(uint32_t idx) {
  Span<char> local = pool[idx].r.local;
  DropRec(idx);
  FreeLocal(local.data() - mirror, local.size());
}

uint32_t MemMeld::RegionTable::IndexAt(uint64_t remote) const {
  return by_remote.Query(remote).value & kIndexMask;
}

uint64_t MemMeld::RegionTable::NextStart(uint64_t at, uint64_t hi) {
  MapleTree::Segment next = by_remote.QueryLowerBound(at);
  return next.value ? std::min(hi, next.begin) : hi;
}

Vec<uint32_t> MemMeld::RegionTable::Overlapping(uint64_t lo, uint64_t hi) {
  Vec<uint32_t> hit;
  for (const MapleTree::Segment& segment : by_remote.QueryRange(lo, PageUp(hi)))
    hit.push_back(segment.value & kIndexMask);
  return hit;
}

std::array<uint32_t, 3> MemMeld::RegionTable::Split(uint32_t idx, uint64_t lo, uint64_t hi) {
  Rec old = pool[idx];
  DropRec(idx);
  uint64_t cuts[] = {old.r.remote, std::max(lo, old.r.remote), std::min(hi, old.End()), old.End()};
  std::array<uint32_t, 3> parts = {};
  for (int i = 0; i < 3; ++i) {
    if (cuts[i] == cuts[i + 1]) continue;
    uint64_t skip = cuts[i] - old.r.remote;
    parts[i] = AddRec(cuts[i], cuts[i + 1] - cuts[i], old.r.prot, old.key + skip, old.path,
                      old.r.file_offset + skip, old.r.local.data() + skip, old.dev, old.inode);
  }
  return parts;
}

void MemMeld::RegionTable::Shrink(uint32_t idx, uint64_t keep) {
  Region& r = pool[idx].r;
  uint64_t size = r.local.size();
  by_remote.Erase(r.remote + keep, r.remote + size);
  by_local.Erase((uint64_t)r.local.data() + keep, (uint64_t)r.local.data() + size);
  FreeLocal(r.local.data() + keep - mirror, size - keep);
  r.local = r.local.first(keep);
}

void MemMeld::RegionTable::Move(uint32_t idx, uint64_t remote) {
  Region& r = pool[idx].r;
  uint64_t local = (uint64_t)r.local.data(), size = r.local.size();
  by_remote.Erase(r.remote, r.remote + size);
  r.remote = remote;
  by_remote.Update(remote, remote + size, PackTranslation(remote, local, idx));
  by_local.Update(local, local + size, PackTranslation(local, remote, idx));
}

MemMeld::~MemMeld() { assert(tracer.load(std::memory_order_relaxed) == 0); }

void MemMeld::Attach(int target_pidfd, Status& status) {
  if (attached) {
    AppendErrorMessage(status) += "MemMeld::Attach is single-use";
    return;
  }
  FD pidfd = fcntl(target_pidfd, F_DUPFD_CLOEXEC, 0);
  if (pidfd < 0) {
    AppendErrorMessage(status) += "MemMeld: invalid pidfd";
    return;
  }
  pid_t pid = 0;
  Str fdinfo = ReadWholeFile(f("/proc/self/fdinfo/{}", (int)pidfd));
  if (size_t p = fdinfo.find("Pid:"); p != Str::npos) pid = atoi(fdinfo.c_str() + p + 4);
  attached = pid != 0;
  if (pid <= 0) {
    AppendErrorMessage(status) += "MemMeld: cannot read pid from pidfd";
    return;
  }
  if (!regions.mirror) {
    AppendErrorMessage(status) += "MemMeld: reserving the local mirror";
    return;
  }

  bool ending = false;
  bool done = false;
  std::map<pid_t, Task> tasks;
  FD maps, mem, pagemap;
  FD memfd;
  long remote_fd = -1;
  dev_t memfd_dev = 0;
  uint64_t memfd_ino = 0;
  uint64_t syscall_insn = 0;
  uint64_t brk = 0;
  Slots slots;
  uint64_t mremaps = 0;
  int moving = 0;
  Vec<Piece> deferred;

  auto Finish = [&](const Status& why) {
    if (done) return;
    done = true;
    if (on_detached) on_detached(why);
  };

  auto OnMapped = [&](const Region& r) {
    if (on_mapped) on_mapped(r);
  };

  auto OnChanged = [&](const Region& before, const Region& after) {
    if (on_changed) on_changed(before, after);
  };

  auto OnUnmapped = [&](const Region& r) {
    if (on_unmapped) on_unmapped(r);
  };

  auto Split = [&](uint64_t lo, uint64_t hi, std::optional<uint64_t> delta = {}) {
    for (uint32_t idx : regions.Overlapping(lo, hi)) {
      const auto& rec = regions.pool[idx];
      if ((rec.r.remote >= lo && rec.End() <= hi) || (delta && rec.Delta() != *delta)) continue;
      OnUnmapped(rec.r);
      for (uint32_t part : regions.Split(idx, lo, hi))
        if (part) OnMapped(regions.pool[part].r);
    }
  };

  auto AddMapping = [&](Mapping m) {
    uint64_t lo = m.remote, hi = lo + m.len, delta = m.key - lo;
    struct stat st = {};
    if (m.file.Opened()) fstat(m.file, &st);
    Split(lo, hi);
    for (uint32_t idx : regions.Overlapping(lo, hi)) {
      auto& rec = regions.pool[idx];
      if (rec.Delta() != delta ||
          (m.file.Opened() && (rec.dev != st.st_dev || rec.inode != st.st_ino))) {
        OnUnmapped(rec.r);
        regions.UnmapRegion(idx);
      } else if (rec.r.prot != m.prot) {
        Region before = rec.r;
        rec.r.prot = m.prot;
        OnChanged(before, rec.r);
      }
    }
    if (m.key >= kFileKey && !m.file.Opened()) return;
    for (uint64_t at = lo; at < hi;) {
      if (uint32_t idx = regions.IndexAt(at)) {
        at = regions.pool[idx].End();
        continue;
      }
      uint64_t end = regions.NextStart(at, hi);
      uint32_t below = at && m.key < kFileKey ? regions.IndexAt(at - 1) : 0;
      if (below && regions.pool[below].Delta() == delta && regions.pool[below].r.prot == m.prot) {
        auto& rec = regions.pool[below];
        Region before = rec.r;
        uint64_t remote = rec.r.remote, key = rec.key, file_offset = rec.r.file_offset;
        Str path = rec.path;
        regions.UnmapRegion(below);
        if (uint32_t idx =
                regions.MapRegion(remote, end - remote, m.prot, key, -1, path, file_offset, memfd))
          OnChanged(before, regions.pool[idx].r);
        else
          OnUnmapped(before);
      } else if (uint32_t idx = regions.MapRegion(at, end - at, m.prot, m.key + (at - lo),
                                                  m.file.Opened() ? (int)m.file : -1, m.path,
                                                  m.file_offset + (at - lo), memfd)) {
        OnMapped(regions.pool[idx].r);
      }
      at = end;
    }
  };

  auto AddPiece = [&](const Piece& p, int prot) {
    bool file = p.key >= kFileKey;
    AddMapping({p.remote,
                p.len,
                prot,
                p.key,
                {},
                file ? Str() : Str(slots.Path(p.key)),
                file ? p.key & ~kFileKey : slots.FileOffset(p.key)});
  };

  auto Shrink = [&](uint32_t idx, uint64_t keep) {
    Region before = regions.pool[idx].r;
    regions.Shrink(idx, keep);
    OnChanged(before, regions.pool[idx].r);
  };

  auto RemovePiece = [&](const Piece& p) {
    uint64_t lo = p.remote, hi = lo + p.len, delta = p.key - lo;
    for (uint32_t idx : regions.Overlapping(lo, hi)) {
      const auto& rec = regions.pool[idx];
      if (rec.Delta() == delta && rec.r.remote < lo && rec.End() <= hi)
        Shrink(idx, lo - rec.r.remote);
    }
    Split(lo, hi, delta);
    for (uint32_t idx : regions.Overlapping(lo, hi)) {
      if (regions.pool[idx].Delta() != delta) continue;
      OnUnmapped(regions.pool[idx].r);
      regions.UnmapRegion(idx);
    }
  };

  auto Memfd = [&](const Vma& v) -> bool {
    return v.shared && memfd_ino && v.inode == memfd_ino && v.dev == memfd_dev;
  };

  auto MemfdIntact = [&](pid_t tid) -> bool {
    return remote_fd >= 0 &&
           LinuxSyscall(__NR_kcmp, tid, getpid(), kKcmpFile, remote_fd, (int)memfd) == 0;
  };

  auto OpenProc = [&]() -> bool {
    maps = open(f("/proc/{}/maps", pid).c_str(), O_RDONLY | O_CLOEXEC);
    mem = open(f("/proc/{}/mem", pid).c_str(), O_RDWR | O_CLOEXEC);
    pagemap = open(f("/proc/{}/pagemap", pid).c_str(), O_RDONLY | O_CLOEXEC);
    return maps.Opened() && mem.Opened() && pagemap.Opened();
  };

  auto Inject = [&](Task& t, long nr, long a = 0, long b = 0, long c = 0, long d = 0, long e = 0,
                    long g = 0) -> long {
    if (t.stop >> 8 == kEventExit) return -ESRCH;
    int pass = 0;
    if (!t.injected) {
      uint64_t all = ~0ull;
      if (LinuxSyscall(__NR_ptrace, kPtraceGetRegs, t.tid, 0, &t.saved) != 0 ||
          LinuxSyscall(__NR_ptrace, kPtraceGetSigmask, t.tid, sizeof t.mask, &t.mask) != 0 ||
          LinuxSyscall(__NR_ptrace, kPtraceSetSigmask, t.tid, sizeof all, &all) != 0)
        return -ESRCH;
      t.injected = true;
      if (t.stop >> 8 == 0 && t.stop != kSyscallStop && t.stop == t.signal)
        std::swap(pass, t.signal);
      PtraceSyscallInfo info;
      if (t.stop == kSyscallStop &&
          LinuxSyscall(__NR_ptrace, kPtraceGetSyscallInfo, t.tid, sizeof info, &info) > 0 &&
          info.op == kSyscallInfoEntry) {
        user_regs_struct cancel = t.saved;
        cancel.orig_rax = -1;
        if (LinuxSyscall(__NR_ptrace, kPtraceSetRegs, t.tid, 0, &cancel) != 0 ||
            t.ResumeAndWait(0) != kSyscallStop)
          return -ESRCH;
        t.rewind = true;
      }
    }
    user_regs_struct r = t.saved;
    r.rax = nr;
    r.orig_rax = -1;
    r.rdi = a, r.rsi = b, r.rdx = c, r.r10 = d, r.r8 = e, r.r9 = g;
    r.rip = syscall_insn;
    if (LinuxSyscall(__NR_ptrace, kPtraceSetRegs, t.tid, 0, &r) != 0) return -ESRCH;
    for (bool entered = false;;) {
      int stop = t.ResumeAndWait(pass);
      pass = 0;
      if (stop < 0) return -ESRCH;
      if (stop == kSyscallStop) {
        PtraceSyscallInfo info;
        if (LinuxSyscall(__NR_ptrace, kPtraceGetSyscallInfo, t.tid, sizeof info, &info) <= 0)
          return -ESRCH;
        if (info.op == kSyscallInfoExit && entered) return info.exit.rval;
        if (info.op == kSyscallInfoEntry) {
          entered = true;
        } else {
          t.saved.rax = info.exit.rval;
          if (LinuxSyscall(__NR_ptrace, kPtraceSetRegs, t.tid, 0, &r) != 0) return -ESRCH;
        }
      } else if (stop >> 8 == 0) {
        t.signal = stop;
      } else if (stop >> 8 == kEventStop && (stop & 0xff) != SIGTRAP) {
        t.stop = stop;
      }
    }
  };

  auto PieceOf = [&](const Vma& v) -> std::optional<Piece> {
    uint64_t len = v.end - v.start;
    if (Memfd(v)) return Piece{v.start, len, v.offset, slots.Epoch(v.offset)};
    if (v.inode) return Piece{v.start, len, kFileKey | v.offset};
    return {};
  };

  auto PiecesIn = [&](uint64_t lo, uint64_t hi) -> Vec<Piece> {
    Vec<Piece> out;
    for (const Vma& v : VmasIn(maps, lo, hi))
      if (std::optional<Piece> p = PieceOf(v)) out.push_back(*p);
    return out;
  };

  auto FreeUnusedPieces = [&](const Call& c, Vec<Piece> gone) {
    std::erase_if(gone, [](const Piece& p) { return p.key >= kFileKey; });
    bool forking = std::ranges::any_of(tasks, [](auto& task) {
      const Call& call = task.second.call;
      return call.nr == __NR_fork ||
             ((call.nr == __NR_clone || call.nr == __NR_clone3) && !(call.clone_flags & kCloneVm));
    });
    if (forking) {
      for (const Piece& p : gone) deferred.push_back(p);
      return;
    }
    Vec<Piece> unsure;
    std::swap(unsure, deferred);
    for (const Piece& p : gone) {
      if (c.moving || mremaps != c.mremaps || slots.Epoch(p.key) != p.epoch)
        unsure.push_back(p);
      else
        slots.Release(p.key, p.len);
    }
    if (unsure.empty()) return;
    Vec<std::pair<uint64_t, uint64_t>> used;
    for (const Vma& v : VmasIn(maps, 0, kUserEnd))
      if (Memfd(v)) used.push_back({v.offset, v.offset + (v.end - v.start)});
    for (auto& [tid, task] : tasks)
      if (const Call& call = task.call; call.offset != kNoOffset)
        used.push_back({call.offset, call.offset + PageUp(call.args[1])});
    for (uint32_t idx : regions.Overlapping(0, kUserEnd))
      if (const auto& rec = regions.pool[idx]; rec.key < kFileKey)
        used.push_back({rec.key, rec.key + rec.r.local.size()});
    std::sort(used.begin(), used.end());
    for (const Piece& p : unsure) {
      uint64_t at = p.key, end = p.key + p.len;
      for (auto [a, b] : used) {
        if (b <= at || a >= end) continue;
        if (a > at) slots.Release(at, a - at);
        at = std::max(at, b);
      }
      if (at < end) slots.Release(at, end - at);
    }
  };

  auto SyncRegions = [&](const Call& c, std::initializer_list<std::pair<uint64_t, uint64_t>> ranges,
                         Vec<Piece> removed) {
    Vec<Piece> shown, lost;
    for (auto [lo, hi] : ranges) {
      Vec<Vma> vmas = VmasIn(maps, lo, hi);
      std::erase_if(vmas, [&](const Vma& v) { return !PieceOf(v); });
      Vec<Piece> stale;
      for (uint32_t idx : regions.Overlapping(lo, hi)) {
        const auto& rec = regions.pool[idx];
        Piece old{rec.r.remote, rec.r.local.size(), rec.key};
        old.epoch = slots.Epoch(old.key);
        uint64_t at = std::max(old.remote, lo), end = std::min(old.remote + old.len, hi);
        for (const Vma& v : vmas) {
          if (PieceOf(v)->key - v.start != rec.Delta() || v.end <= at || v.start >= end ||
              (!Memfd(v) && (v.dev != rec.dev || v.inode != rec.inode)))
            continue;
          if (v.start > at) stale.push_back(old.Part(at, v.start));
          at = std::max(at, v.end);
        }
        if (at < end) stale.push_back(old.Part(at, end));
      }
      for (const Piece& p : stale) RemovePiece(p);
      for (const Vma& v : vmas) {
        Piece piece = *PieceOf(v);
        shown.push_back(piece);
        if (v.prot == PROT_NONE) {
          RemovePiece(piece);
          continue;
        }
        Vec<std::pair<uint64_t, uint64_t>> guards = GuardRuns(pagemap, v.start, v.end);
        for (auto [from, to] : guards) RemovePiece(piece.Part(from, to));
        uint64_t at = v.start;
        for (auto [from, to] : guards) {
          if (at < from) AddPiece(piece.Part(at, from), v.prot);
          at = to;
        }
        if (at < v.end) AddPiece(piece.Part(at, v.end), v.prot);
      }
      for (const Piece& p : stale) lost.push_back(p);
    }
    for (const Piece& p : lost) removed = PiecesWithout(removed, p.key, p.len);
    for (const Piece& p : lost) removed.push_back(p);
    for (const Piece& p : shown) removed = PiecesWithout(removed, p.key, p.len);
    FreeUnusedPieces(c, std::move(removed));
  };

  auto Privatize = [&](Task& t, int maps_fd, int mem_fd, int pagemap_fd, uint64_t lo = 0,
                       uint64_t hi = kUserEnd) {
    for (const Vma& v : VmasIn(maps_fd, lo, hi)) {
      if (!Memfd(v)) continue;
      uint64_t len = v.end - v.start;
      Vec<std::pair<uint64_t, uint64_t>> guards;
      if (pagemap_fd >= 0) guards = GuardRuns(pagemap_fd, v.start, v.end);
      if (Inject(t, __NR_mmap, v.start, len, v.prot,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0) != (long)v.start)
        continue;
      void* local = mmap(nullptr, len, PROT_READ, MAP_SHARED, memfd, v.offset);
      if (local == MAP_FAILED) continue;
      ForEachExtent(memfd, v.offset, len, [&](uint64_t at, uint64_t n) {
        for (uint64_t written = 0; written < n;) {
          ssize_t w = pwrite(mem_fd, (char*)local + (at - v.offset) + written, n - written,
                             v.start + (at - v.offset) + written);
          if (w < 0 && errno == EINTR) continue;
          if (w <= 0) break;
          written += w;
        }
      });
      munmap(local, len);
      for (auto [from, to] : guards) Inject(t, __NR_madvise, from, to - from, kMadvGuardInstall);
    }
  };

  auto OpenFile = [&](uint64_t start, uint64_t end, StrView path, dev_t dev, uint64_t inode,
                      bool write) -> FD {
    for (const Str& name :
         {f("/proc/{}/map_files/{:x}-{:x}", pid, start, end), f("/proc/{}/root{}", pid, path)})
      for (int mode : {write ? O_RDWR : O_RDONLY, O_RDONLY}) {
        FD file = open(name.c_str(), mode | O_CLOEXEC);
        struct stat st;
        if (file.Opened() && fstat(file, &st) == 0 && S_ISREG(st.st_mode) && st.st_dev == dev &&
            st.st_ino == inode)
          return file;
      }
    return {};
  };

  auto Convert = [&](Task& t, uint64_t start, uint64_t len, int prot, bool file, StrView path = {},
                     uint64_t file_offset = 0) -> uint64_t {
    uint64_t offset =
        MemfdIntact(t.tid) ? slots.Take(len, len, file, path, file_offset) : kNoOffset;
    if (offset == kNoOffset) return kNoOffset;
    CopyIntoMirror(memfd, offset, mem, pagemap, start, len, file);
    if (Inject(t, __NR_mmap, start, len, prot, MAP_SHARED | MAP_FIXED, remote_fd, offset) ==
        (long)start)
      return offset;
    slots.Release(offset, len);
    return kNoOffset;
  };

  auto ConvertFile = [&](Task& t, uint64_t start, uint64_t len) {
    Vma v;
    Str path;
    if (!QueryVma(maps, start, v, &path) || v.start > start || v.end < start + len || v.shared ||
        !v.inode || Memfd(v))
      return;
    uint64_t file_offset = v.offset + (start - v.start);
    uint64_t offset = Convert(t, start, len, v.prot, true, path, file_offset);
    t.Restore();
    if (offset != kNoOffset && v.prot != PROT_NONE)
      AddMapping({start, len, v.prot, offset, {}, path, file_offset});
  };

  auto AddFileRegion = [&](Task& t, uint64_t start, uint64_t len, FD file) {
    Vma v;
    Str path;
    if (!QueryVma(maps, start, v, &path) || v.start > start || v.end < start + len || !v.inode ||
        Memfd(v) || v.prot == PROT_NONE)
      return;
    if (file.Opened() && !v.shared)
      file = open(f("/proc/self/fd/{}", (int)file).c_str(), O_RDONLY | O_CLOEXEC);
    if (!file.Opened()) file = OpenFile(v.start, v.end, path, v.dev, v.inode, v.shared);
    uint64_t file_offset = v.offset + (start - v.start);
    struct stat st;
    if (!file.Opened() || fstat(file, &st) != 0 || !S_ISREG(st.st_mode) || st.st_dev != v.dev ||
        st.st_ino != v.inode || (!v.shared && file_offset + len > PageUp(st.st_size)))
      return v.shared ? void() : ConvertFile(t, start, len);
    AddMapping({start, len, v.prot, kFileKey | file_offset, std::move(file), path, file_offset});
  };

  auto CreateMemfd = [&](Task& t) -> bool {
    long scratch =
        Inject(t, __NR_mmap, 0, kPage, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (IsError(scratch)) return false;
    long fd = pwrite(mem, "pm-mirror", 10, scratch) == 10
                  ? Inject(t, __NR_memfd_create, scratch, MFD_CLOEXEC)
                  : -EIO;
    Inject(t, __NR_munmap, scratch, kPage);
    if (IsError(fd)) return false;
    remote_fd = Inject(t, __NR_fcntl, fd, F_DUPFD_CLOEXEC, kRemoteFd);
    if (IsError(remote_fd))
      remote_fd = fd;
    else
      Inject(t, __NR_close, fd);
    struct stat st;
    if (Inject(t, __NR_ftruncate, remote_fd, kMemfdSize) != 0 ||
        (memfd = (int)LinuxSyscall(__NR_pidfd_getfd, (int)pidfd, (int)remote_fd, 0)) < 0 ||
        fstat(memfd, &st) != 0) {
      Inject(t, __NR_close, remote_fd);
      remote_fd = -1;
      return false;
    }
    memfd_dev = st.st_dev;
    memfd_ino = st.st_ino;
    slots.memfd = memfd;
    return true;
  };

  auto ConvertAddressSpace = [&](Task& t) -> bool {
    Str smaps = ReadWholeFile(f("/proc/{}/smaps", pid));
    Vec<MapEntry> vmas;
    for (StrView text = smaps; !text.empty();) {
      size_t nl = text.find('\n');
      StrView line = nl == StrView::npos ? text : text.substr(0, nl);
      text = nl == StrView::npos ? StrView() : text.substr(nl + 1);
      uint64_t start, end, offset, inode;
      unsigned major, minor;
      char perms[8] = {};
      int name_at = 0;
      if (sscanf(Str(line).c_str(), "%lx-%lx %7s %lx %x:%x %lu %n", &start, &end, perms, &offset,
                 &major, &minor, &inode, &name_at) >= 7 &&
          perms[0]) {
        MapEntry e;
        e.start = start;
        e.end = end;
        e.file_offset = offset;
        e.dev = makedev(major, minor);
        e.inode = inode;
        e.prot = (perms[0] == 'r' ? PROT_READ : 0) | (perms[1] == 'w' ? PROT_WRITE : 0) |
                 (perms[2] == 'x' ? PROT_EXEC : 0);
        e.shared = perms[3] == 's';
        if (name_at > 0 && (size_t)name_at <= line.size()) e.path = line.substr(name_at);
        vmas.push_back(e);
      } else if (line.starts_with("VmFlags:") && !vmas.empty()) {
        vmas.back().vm_flags = line.substr(sizeof("VmFlags:") - 1);
      } else if (line.starts_with("Anonymous:") && !vmas.empty()) {
        vmas.back().anonymous = strtoull(Str(line.substr(10)).c_str(), nullptr, 10) * 1024;
      }
    }
    syscall_insn = 0;
    for (const MapEntry& v : vmas) {
      if (v.path != "[vdso]") continue;
      Vec<uint8_t> code(v.end - v.start);
      if (pread(mem, code.data(), code.size(), v.start) == (ssize_t)code.size())
        for (size_t i = 0; i + 1 < code.size() && !syscall_insn; ++i)
          if (code[i] == 0x0f && code[i + 1] == 0x05) syscall_insn = v.start + i;
    }
    if (!syscall_insn || !CreateMemfd(t)) {
      t.Restore();
      return false;
    }
    brk = PageUp(Inject(t, __NR_brk, 0));
    Vec<std::pair<uint64_t, uint64_t>> guards = GuardRuns(pagemap, 0, kUserEnd - kPage);
    if (!guards.empty()) {
      Vec<MapEntry> parts;
      for (const MapEntry& v : vmas) {
        uint64_t at = v.start;
        auto Keep = [&](uint64_t end) {
          if (at >= end) return;
          MapEntry part = v;
          part.start = at;
          part.end = end;
          if (v.inode) part.file_offset += at - v.start;
          parts.push_back(part);
        };
        for (auto [lo, hi] : guards) {
          if (hi <= v.start || lo >= v.end) continue;
          Keep(std::max(lo, v.start));
          at = std::min(hi, v.end);
        }
        Keep(v.end);
      }
      vmas = std::move(parts);
    }
    Vec<Mapping> mappings;
    for (size_t i = 0; i < vmas.size(); ++i) {
      const MapEntry& v = vmas[i];
      uint64_t len = v.end - v.start;
      bool file = v.inode != 0;
      if (v.path == "[vdso]" || v.path == "[vvar]" || v.path == "[vvar_vclock]" ||
          v.path == "[vsyscall]" || v.HasFlag("wf") || v.HasFlag("dc") || v.HasFlag("ht") ||
          v.HasFlag("io") || v.HasFlag("pf") || v.HasFlag("um") || v.HasFlag("uw") ||
          (v.HasFlag("gd") && v.path != "[stack]"))
        continue;
      if (v.shared) {
        if (FD f = OpenFile(v.start, v.end, v.path, v.dev, v.inode, true);
            f.Opened() && v.prot != PROT_NONE)
          mappings.push_back({v.start, len, v.prot, kFileKey | v.file_offset, std::move(f),
                              Str(v.path), v.file_offset});
        continue;
      }
      if (v.path == "[stack]") {
        uint64_t below = i ? vmas[i - 1].end : 0;
        Str stat = ReadWholeFile(f("/proc/{}/stat", pid));
        uint64_t arg_start = 0;
        if (size_t paren = stat.rfind(')'); paren != Str::npos) {
          StrView rest = StrView(stat).substr(paren + 2);
          for (int field = 3; field < 48 && !rest.empty(); ++field)
            rest.remove_prefix(rest.find(' ') + 1);
          arg_start = strtoull(Str(rest.substr(0, rest.find(' '))).c_str(), nullptr, 10);
        }
        uint64_t split = arg_start >= v.start && arg_start < v.end ? PageDown(arg_start) : v.end;
        rlimit limit = {};
        LinuxSyscall(__NR_prlimit64, pid, RLIMIT_STACK, nullptr, &limit);
        uint64_t size = limit.rlim_cur == RLIM_INFINITY ? 64ull << 20 : PageUp(limit.rlim_cur);
        uint64_t low =
            std::min(v.start, std::max(v.end > size ? v.end - size : 0, below + kStackGuard));
        if (split <= v.start || !MemfdIntact(t.tid)) continue;
        uint64_t offset = slots.Take(split - low, split - low, false, "[stack]");
        if (offset == kNoOffset) continue;
        CopyIntoMirror(memfd, offset + (v.start - low), mem, pagemap, v.start, split - v.start,
                       false);
        Inject(t, __NR_mmap, low - kStackGuard, kStackGuard, PROT_NONE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE, -1, 0);
        if (Inject(t, __NR_mmap, low, split - low, v.prot, MAP_SHARED | MAP_FIXED, remote_fd,
                   offset) == (long)low)
          mappings.push_back({low, split - low, v.prot, offset, {}, Str(v.path), 0});
        else
          slots.Release(offset, split - low);
        continue;
      }
      if (file && !(v.prot & PROT_WRITE) && !v.anonymous) {
        FD f = OpenFile(v.start, v.end, v.path, v.dev, v.inode, false);
        struct stat st;
        if (f.Opened() && fstat(f, &st) == 0 && v.file_offset + len <= PageUp(st.st_size)) {
          if (v.prot != PROT_NONE)
            mappings.push_back({v.start, len, v.prot, kFileKey | v.file_offset, std::move(f),
                                Str(v.path), v.file_offset});
          continue;
        }
      }
      uint64_t offset = Convert(t, v.start, len, v.prot, file, v.path, v.file_offset);
      if (offset != kNoOffset && v.prot != PROT_NONE)
        mappings.push_back(
            {v.start, len, v.prot, offset, {}, Str(v.path), file ? v.file_offset : 0});
    }
    t.Restore();
    for (uint32_t idx : regions.Overlapping(0, kUserEnd)) {
      OnUnmapped(regions.pool[idx].r);
      regions.UnmapRegion(idx);
    }
    regions.high_water.store(0, std::memory_order_relaxed);
    regions.free_list.clear();
    for (const Mapping& m : mappings)
      if (uint32_t idx =
              regions.MapRegion(m.remote, m.len, m.prot, m.key, m.file.Opened() ? (int)m.file : -1,
                                m.path, m.file_offset, memfd))
        OnMapped(regions.pool[idx].r);
    if (on_attached) on_attached();
    return true;
  };

  auto RemoveTask = [&](pid_t tid) {
    auto it = tasks.find(tid);
    if (it == tasks.end()) return;
    if (it->second.call.counted) --moving;
    tasks.erase(it);
  };

  auto OnTaskExit = [&](pid_t tid, int code) {
    auto it = tasks.find(tid);
    bool known = it != tasks.end() && it->second.known;
    RemoveTask(tid);
    if (!known || std::ranges::any_of(tasks, [](auto& task) { return task.second.known; })) return;
    Status why;
    if (WIFSIGNALED(code))
      why = ErrorStatus(f("the target was killed by signal {}", WTERMSIG(code)));
    else if (WEXITSTATUS(code))
      why = ErrorStatus(f("the target exited with status {}", WEXITSTATUS(code)));
    Finish(why);
  };

  auto Resume = [&](Task& t) {
    if (t.AtExitStop()) {
      unsigned long code = 0;
      LinuxSyscall(__NR_ptrace, kPtraceGetEventMsg, t.tid, 0, &code);
      LinuxSyscall(__NR_ptrace, kPtraceDetach, t.tid, 0, 0);
      return OnTaskExit(t.tid, (int)code);
    }
    LinuxSyscall(__NR_ptrace, kPtraceSyscall, t.tid, 0, t.signal);
    t.signal = 0;
    t.running = true;
  };

  auto NewChild = [&](Task& t) {
    unsigned long child = 0;
    LinuxSyscall(__NR_ptrace, kPtraceGetEventMsg, t.tid, 0, &child);
    auto [it, fresh] = tasks.try_emplace(child);
    Task& c = it->second;
    c.tid = child;
    if (fresh && (c.stop = WaitStop(child)) < 0) return RemoveTask(child);
    c.running = false;
    if (LinuxSyscall(__NR_kcmp, t.tid, child, kKcmpVm, 0, 0) == 0) {
      c.known = true;
      return Resume(c);
    }
    if (remote_fd < 0) {
      LinuxSyscall(__NR_ptrace, kPtraceDetach, child, 0, 0);
      return RemoveTask(child);
    }
    FD child_pidfd((int)LinuxSyscall(__NR_pidfd_open, child, 0));
    bool own_files = LinuxSyscall(__NR_kcmp, child, pid, kKcmpFiles, 0, 0) != 0;
    {
      // Let the child go
      const Call& call = t.call;
      FD child_maps = open(f("/proc/{}/maps", child).c_str(), O_RDONLY | O_CLOEXEC);
      FD child_mem = open(f("/proc/{}/mem", child).c_str(), O_RDWR | O_CLOEXEC);
      Privatize(c, child_maps, child_mem, -1);
      if (own_files && MemfdIntact(child)) Inject(c, __NR_close, remote_fd);
      if (call.clone_flags & (kCloneChildSetTid | kCloneChildClearTid)) {
        int set_tid = (int)Inject(c, __NR_set_tid_address,
                                  call.clone_flags & kCloneChildClearTid ? call.child_tid : 0);
        if (call.clone_flags & kCloneChildSetTid)
          pwrite(child_mem, &set_tid, sizeof set_tid, call.child_tid);
        if (call.nr == __NR_clone3)
          pwrite(child_mem, &call.clone_flags, sizeof call.clone_flags, call.args[0]);
        else
          c.saved.rdi = call.clone_flags;
      }
      c.Restore();
      LinuxSyscall(__NR_ptrace, kPtraceDetach, child, 0, c.signal);
      tasks.erase(child);
    }
    if (child_pidfd.Opened() && own_files && !ending && on_forked) on_forked(child_pidfd);
  };

  auto StopAll = [&] {
    for (;;) {
      Vec<pid_t> running;
      for (auto& [tid, t] : tasks)
        if (t.running) running.push_back(tid);
      if (running.empty()) return;
      for (pid_t tid : running) LinuxSyscall(__NR_ptrace, kPtraceInterrupt, tid, 0, 0);
      for (pid_t tid : running) {
        int stop = WaitStop(tid);
        auto it = tasks.find(tid);
        if (it == tasks.end()) continue;
        if (stop < 0) {
          RemoveTask(tid);
          continue;
        }
        Task& t = it->second;
        t.running = false;
        t.stop = stop;
        if (stop >> 8 == 0 && stop != kSyscallStop) t.signal = stop;
        if (stop >> 8 == kEventClone || stop >> 8 == kEventFork) {
          NewChild(t);
          t.stop = kInterruptStop;
        }
        if (stop >> 8 == kEventExec) t.stop = kInterruptStop;
      }
    }
  };

  auto DetachAll = [&] {
    StopAll();
    for (auto& [tid, t] : tasks) LinuxSyscall(__NR_ptrace, kPtraceDetach, tid, 0, t.signal);
    tasks.clear();
  };

  auto Seize = [&]() -> bool {
    Vec<pid_t> seen;
    for (bool added = true; added;) {
      added = false;
      Vec<pid_t> listed;
      if (DIR* d = opendir(f("/proc/{}/task", pid).c_str())) {
        while (dirent* e = readdir(d))
          if (e->d_name[0] != '.') listed.push_back(atoi(e->d_name));
        closedir(d);
      }
      for (pid_t tid : listed) {
        if (seen.Contains(tid)) continue;
        seen.push_back(tid);
        long seized = LinuxSyscall(__NR_ptrace, kPtraceSeize, tid, 0, kOptions);
        if (seized != 0 && LinuxSyscall(__NR_ptrace, kPtraceInterrupt, tid, 0, 0) != 0) {
          if (seized == -ESRCH) continue;
          errno = -seized;
          return false;
        }
        Task& t = tasks[tid];
        t.tid = tid;
        t.known = true;
        added = true;
      }
      StopAll();
    }
    return !tasks.empty();
  };

  auto DetachCleanly = [&](Status why) {
    ending = true;
    StopAll();
    for (auto& [tid, t] : tasks) {
      if (!t.Injectable()) continue;
      Privatize(t, maps, mem, pagemap);
      if (MemfdIntact(tid)) Inject(t, __NR_close, remote_fd);
      t.Restore();
      break;
    }
    DetachAll();
    Finish(why);
  };

  auto DetachIfMemfdLost = [&](pid_t tid) -> bool {
    if (MemfdIntact(tid)) return false;
    DetachCleanly(ErrorStatus("the target closed the memfd descriptor"));
    return true;
  };

  auto Intercepts = [&](uint64_t nr, const uint64_t (&a)[6]) -> bool {
    switch (nr) {
      case __NR_mmap:
      case __NR_munmap:
      case __NR_mremap:
      case __NR_mprotect:
      case __NR_pkey_mprotect:
      case __NR_fork:
        return true;
      case __NR_brk:
        return a[0] != 0;
      case __NR_seccomp:
        return a[0] == kSeccompSetModeStrict || a[0] == kSeccompSetModeFilter;
      case __NR_madvise:
        return a[2] == MADV_DONTNEED || a[2] == MADV_FREE || a[2] == MADV_DONTNEED_LOCKED ||
               a[2] == MADV_WIPEONFORK || a[2] == kMadvGuardInstall || a[2] == kMadvGuardRemove;
      case __NR_clone:
        return !(a[0] & kCloneVm) || (a[0] & kCloneUntraced);
      case __NR_clone3: {
        uint64_t flags = kCloneVm;
        pread(mem, &flags, sizeof flags, a[0]);
        return !(flags & kCloneVm) || (flags & kCloneUntraced);
      }
      case __NR_prctl:
        return a[0] == PR_SET_SECCOMP;
    }
    return false;
  };

  auto OnSyscallEntry = [&](Task& t) {
    Call& c = t.call;
    uint64_t lo = c.args[0], hi = lo + PageUp(c.args[1]);
    switch (c.nr) {
      case __NR_mmap: {
        uint64_t len = PageUp(c.args[1]), flags = c.args[3];
        if ((flags & MAP_FIXED) && !(flags & MAP_FIXED_NOREPLACE)) c.pieces = PiecesIn(lo, hi);
        if ((flags & MAP_TYPE) != MAP_PRIVATE || !(flags & MAP_ANONYMOUS) ||
            (flags & (kMapGrowsDown | MAP_HUGETLB)) || !len || len > kMemfdSize / 2)
          return;
        if (DetachIfMemfdLost(t.tid)) return;
        c.offset = slots.Take(len, len);
        if (c.offset == kNoOffset) return;
        c.epoch = slots.Epoch(c.offset);
        return t.SetRegs([&](user_regs_struct& r) {
          r.r10 = (flags & ~(uint64_t)(MAP_TYPE | MAP_ANONYMOUS)) | MAP_SHARED;
          r.r8 = remote_fd;
          r.r9 = c.offset;
        });
      }
      case __NR_munmap:
        c.pieces = PiecesIn(lo, hi);
        return;
      case __NR_mremap: {
        uint64_t old_len = PageUp(c.args[1]), new_len = PageUp(c.args[2]), flags = c.args[3];
        c.pieces = PiecesIn(lo, lo + old_len);
        if (flags & MREMAP_FIXED)
          for (const Piece& p : PiecesIn(c.args[4], c.args[4] + new_len)) c.pieces.push_back(p);
        if (QueryVma(maps, lo, c.source) && c.source.start <= lo && c.source.end >= lo + old_len) {
          c.source.offset += lo - c.source.start;
          c.source.start = lo;
          c.source.end = lo + old_len;
        } else {
          c.source = {};
        }
        if (Memfd(c.source) && (!old_len || (flags & kMremapDontUnmap))) return t.Cancel(EINVAL);
        uint64_t o = c.source.offset;
        if (Memfd(c.source) && new_len > old_len &&
            !slots.CanExtend(o + old_len, new_len - old_len)) {
          if (DetachIfMemfdLost(t.tid)) return;
          if (new_len > kSlot) return t.Cancel(ENOMEM);
          uint64_t to =
              slots.Take(old_len, new_len, slots.File(o), slots.Path(o), slots.FileOffset(o));
          if (to == kNoOffset) return t.Cancel(ENOMEM);
          ForEachExtent(memfd, o, old_len, [&](uint64_t at, uint64_t n) {
            loff_t from = at, into = to + (at - o);
            while (n) {
              ssize_t copied = copy_file_range(memfd, &from, memfd, &into, n, 0);
              if (copied < 0 && errno == EINTR) continue;
              if (copied <= 0) break;
              n -= copied;
            }
          });
          if (Inject(t, __NR_mmap, lo, old_len, c.source.prot, MAP_SHARED | MAP_FIXED, remote_fd,
                     to) != (long)lo) {
            slots.Release(to, old_len);
            t.rewind = false;
            t.saved.rax = -ENOMEM;
            t.Restore();
            t.call = {};
            return;
          }
          t.Restore();
          Split(lo, lo + old_len, o - lo);
          for (uint32_t idx : regions.Overlapping(lo, lo + old_len)) {
            auto& rec = regions.pool[idx];
            if (rec.Delta() != o - lo) continue;
            rec.key += to - o;
            if (mmap(rec.r.local.data(), rec.r.local.size(), PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_FIXED, memfd, rec.key) != MAP_FAILED)
              continue;
            OnUnmapped(rec.r);
            regions.UnmapRegion(idx);
          }
          FreeUnusedPieces(c, {Piece{lo, old_len, o, slots.Epoch(o)}});
          t.call = {};
          return;
        }
        c.counted = true;
        c.moving = moving++ > 0;
        c.mremaps = ++mremaps;
        return;
      }
      case __NR_brk:
        if (PageUp(c.args[0]) < brk) c.pieces = PiecesIn(PageUp(c.args[0]), brk);
        return;
      case __NR_madvise: {
        if (c.args[2] == kMadvGuardInstall || c.args[2] == kMadvGuardRemove) return;
        Vec<Vma> vmas = VmasIn(maps, lo, hi);
        if (vmas.empty() || !std::ranges::all_of(vmas, [&](const Vma& v) {
              return Memfd(v) && !slots.File(v.offset);
            }))
          return;
        if (c.args[2] != MADV_WIPEONFORK)
          return t.SetRegs([](user_regs_struct& r) { r.rdx = MADV_REMOVE; });
        Vec<Piece> pieces = PiecesIn(lo, hi);
        Privatize(t, maps, mem, pagemap, lo, hi);
        t.Restore();
        return SyncRegions(c, {{lo, hi}}, pieces);
      }
      case __NR_clone:
        c.clone_flags = c.args[0];
        c.child_tid = c.args[3];
        return t.SetRegs([&](user_regs_struct& r) { r.rdi &= ~Stripped(c.clone_flags); });
      case __NR_clone3: {
        uint64_t args[3];
        if (pread(mem, args, sizeof args, c.args[0]) != sizeof args) return;
        c.clone_flags = args[0];
        c.child_tid = args[2];
        args[0] &= ~Stripped(args[0]);
        pwrite(mem, args, sizeof args[0], c.args[0]);
        return;
      }
      case __NR_seccomp:
      case __NR_prctl:
        return DetachCleanly(ErrorStatus("the target installs a seccomp filter"));
    }
  };

  auto OnSyscallExit = [&](Task& t, Call& c, long result) {
    if (c.result) return t.SetRegs([&](user_regs_struct& r) { r.rax = c.result; });
    uint64_t lo = c.args[0], len = PageUp(c.args[1]);
    bool stripped = c.clone_flags & Stripped(c.clone_flags);
    switch (c.nr) {
      case __NR_mmap: {
        uint64_t flags = c.args[3];
        int prot = (int)c.args[2] & (PROT_READ | PROT_WRITE | PROT_EXEC);
        if (c.offset != kNoOffset)
          t.SetRegs([&](user_regs_struct& r) {
            r.r10 = flags;
            r.r8 = c.args[4];
            r.r9 = c.args[5];
          });
        if (IsError(result)) {
          if (c.offset != kNoOffset) slots.Release(c.offset, len);
          return;
        }
        bool shared = (flags & MAP_TYPE) != MAP_PRIVATE, anonymous = flags & MAP_ANONYMOUS;
        if (c.offset != kNoOffset)
          c.pieces.push_back({(uint64_t)result, len, c.offset, c.epoch});
        else if (!shared && !anonymous && (prot & PROT_WRITE))
          ConvertFile(t, result, len);
        else if ((shared || !anonymous) && prot != PROT_NONE)
          AddFileRegion(
              t, result, len,
              anonymous ? FD()
                        : FD((int)LinuxSyscall(__NR_pidfd_getfd, (int)pidfd, (int)c.args[4], 0)));
        return SyncRegions(c, {{(uint64_t)result, result + len}}, std::move(c.pieces));
      }
      case __NR_munmap:
        if (!IsError(result)) SyncRegions(c, {{lo, lo + len}}, c.pieces);
        return;
      case __NR_mremap: {
        if (IsError(result)) return;
        uint64_t old_len = len, new_len = PageUp(c.args[2]), to = result;
        if (to != lo) {
          Split(to, to + new_len);
          for (uint32_t idx : regions.Overlapping(to, to + new_len)) {
            const auto& rec = regions.pool[idx];
            c.pieces.push_back({rec.r.remote, rec.r.local.size(), rec.key, slots.Epoch(rec.key)});
            OnUnmapped(rec.r);
            regions.UnmapRegion(idx);
          }
          uint64_t moved = std::min(old_len, new_len);
          Split(lo, lo + old_len);
          for (uint32_t idx : regions.Overlapping(lo, lo + moved)) {
            const auto& rec = regions.pool[idx];
            if (rec.End() > lo + moved) Shrink(idx, lo + moved - rec.r.remote);
            Region before = rec.r;
            regions.Move(idx, before.remote + (to - lo));
            OnChanged(before, rec.r);
          }
        }
        if (Memfd(c.source) && new_len > old_len) {
          slots.Use(c.source.offset + old_len, new_len - old_len);
        } else if (!Memfd(c.source) && c.source.inode && c.source.prot != PROT_NONE) {
          if (!c.source.shared && ((c.source.prot & PROT_WRITE) || new_len > old_len))
            ConvertFile(t, to, new_len);
          else
            AddFileRegion(t, to, new_len, {});
        }
        return SyncRegions(c, {{lo, lo + old_len}, {to, to + new_len}}, std::move(c.pieces));
      }
      case __NR_mprotect:
      case __NR_pkey_mprotect:
        for (const Vma& v : VmasIn(maps, lo, lo + len)) {
          if (Memfd(v) || !v.inode || v.prot == PROT_NONE) continue;
          if (!v.shared && (v.prot & PROT_WRITE))
            ConvertFile(t, v.start, v.end - v.start);
          else
            AddFileRegion(t, v.start, v.end - v.start, {});
        }
        return SyncRegions(c, {{lo, lo + len}}, {});
      case __NR_brk: {
        uint64_t end = PageUp(result);
        if (end > brk && MemfdIntact(t.tid)) {
          uint64_t grown = end - brk, offset = kNoOffset;
          Vma below;
          if (QueryVma(maps, brk - 1, below) && below.start < brk && below.end == brk &&
              Memfd(below)) {
            uint64_t continued = below.offset + (brk - below.start);
            if (slots.CanExtend(continued, grown)) {
              slots.Use(continued, grown);
              offset = continued;
            }
          }
          if (offset == kNoOffset) offset = slots.Take(grown, kSlot, false, "[heap]");
          if (offset != kNoOffset) {
            long r = Inject(t, __NR_mmap, brk, grown, PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_FIXED, remote_fd, offset);
            t.Restore();
            if (r == (long)brk)
              SyncRegions(c, {{brk, end}}, {});
            else
              slots.Release(offset, grown);
          }
        } else if (end < brk) {
          SyncRegions(c, {{end, brk}}, std::move(c.pieces));
        }
        brk = end;
        return;
      }
      case __NR_madvise:
        t.SetRegs([&](user_regs_struct& r) { r.rdx = c.args[2]; });
        if (c.args[2] == kMadvGuardInstall || c.args[2] == kMadvGuardRemove)
          SyncRegions(c, {{lo, lo + len}}, {});
        return;
      case __NR_clone:
        if (stripped) t.SetRegs([&](user_regs_struct& r) { r.rdi = c.clone_flags; });
        return FreeUnusedPieces(c, {});
      case __NR_clone3:
        if (stripped) pwrite(mem, &c.clone_flags, sizeof c.clone_flags, c.args[0]);
        return FreeUnusedPieces(c, {});
      case __NR_fork:
        return FreeUnusedPieces(c, {});
    }
  };

  auto OnStop = [&](pid_t tid, int stop) {
    Task& t = tasks[tid];
    t.tid = tid;
    t.running = false;
    t.stop = stop;
    if (stop >> 8 == kEventExec) {
      unsigned long former = 0;
      LinuxSyscall(__NR_ptrace, kPtraceGetEventMsg, tid, 0, &former);
      if (auto it = tasks.find(former); (pid_t)former != tid && it != tasks.end()) {
        t.known = it->second.known;
        tasks.erase(it);
      }
    }
    if (!t.known) return;
    if (stop == kSyscallStop) {
      PtraceSyscallInfo info;
      if (LinuxSyscall(__NR_ptrace, kPtraceGetSyscallInfo, tid, sizeof info, &info) > 0) {
        if (info.op == kSyscallInfoExit && t.call.nr >= 0) {
          Call c = std::move(t.call);
          t.call = {};
          if (c.counted) --moving;
          OnSyscallExit(t, c, info.exit.rval);
        } else if (info.op == kSyscallInfoEntry && info.arch == kAuditArchX86_64 &&
                   Intercepts(info.entry.nr, (const uint64_t (&)[6])info.entry.args)) {
          t.call = {.nr = (long)info.entry.nr, .mremaps = mremaps, .moving = moving > 0};
          std::copy_n(info.entry.args, 6, t.call.args);
          OnSyscallEntry(t);
        }
      }
    } else if (stop >> 8 == 0) {
      t.signal = stop;
    } else if (stop >> 8 == kEventStop && (stop & 0xff) != SIGTRAP) {
      LinuxSyscall(__NR_ptrace, kPtraceListen, tid, 0, 0);
      t.running = true;
      return;
    } else if (stop >> 8 == kEventClone || stop >> 8 == kEventFork) {
      NewChild(t);
    } else if (stop >> 8 == kEventExec) {
      if (tid != pid) {
        LinuxSyscall(__NR_ptrace, kPtraceDetach, tid, 0, 0);
        return RemoveTask(tid);
      }
      t = {.tid = tid, .known = true, .running = false, .stop = t.stop};
      StopAll();
      for (auto it = tasks.begin(); it != tasks.end();) {
        if (it->first == tid) {
          ++it;
          continue;
        }
        LinuxSyscall(__NR_ptrace, kPtraceDetach, it->first, 0, it->second.signal);
        it = tasks.erase(it);
      }
      moving = 0;
      remote_fd = -1;
      memfd_ino = 0;
      slots = {};
      deferred.clear();
      if (!OpenProc() || !ConvertAddressSpace(t))
        DetachCleanly(ErrorStatus("converting the new program's memory"));
    }
    if (auto it = tasks.find(tid); it != tasks.end() && !it->second.running) Resume(it->second);
  };

  sigset_t previous;
  {
    // Wake signal
    struct sigaction handler = {};
    handler.sa_handler = [](int) {};
    sigaction(WakeSignal(), &handler, nullptr);
    sigset_t wake;
    sigemptyset(&wake);
    sigaddset(&wake, WakeSignal());
    pthread_sigmask(SIG_UNBLOCK, &wake, &previous);
    tracer.store(gettid(), std::memory_order_relaxed);
  }
  {
    // Attach
    const char* failure = nullptr;
    if (ReadWholeFile(f("/proc/{}/status", pid)).contains("Seccomp:\t2"))
      Finish(ErrorStatus("the target runs under a seccomp filter"));
    else if (!OpenProc())
      failure = "opening the target's /proc files";
    else if (!Seize())
      failure = "seizing the target";
    else if (LinuxSyscall(__NR_pidfd_send_signal, (int)pidfd, 0, nullptr, 0) != 0)
      failure = "the pidfd no longer names the seized process";
    else if (auto it = std::ranges::find_if(tasks, [](auto& t) { return t.second.Injectable(); });
             it == tasks.end() || !ConvertAddressSpace(it->second))
      failure = "creating the memfd in the target";
    if (failure) {
      Status why;
      AppendErrorMessage(why) += failure;
      Finish(why);
      DetachAll();
    }
    Vec<pid_t> stopped;
    for (auto& [tid, t] : tasks) stopped.push_back(tid);
    for (pid_t tid : stopped)
      if (auto it = tasks.find(tid); it != tasks.end() && !done) OnStop(tid, it->second.stop);
  }
  while (!done) {
    siginfo_t info = {};
    {
      // Wait for the next event
      if (!detach.load(std::memory_order_relaxed) &&
          LinuxSyscall(__NR_waitid, kWaitAll, 0, &info, WSTOPPED | WEXITED | __WALL | kWaitNoThread,
                       nullptr) == -ECHILD)
        pause();
    }
    {
      // Handle the event
      if (info.si_code == CLD_TRAPPED)
        OnStop(info.si_pid, info.si_status);
      else if (info.si_pid)
        OnTaskExit(info.si_pid, info.si_code == CLD_EXITED ? info.si_status << 8 : info.si_status);
      else if (detach.load(std::memory_order_relaxed))
        DetachCleanly({});
    }
  }
  pthread_sigmask(SIG_SETMASK, &previous, nullptr);
  tracer.store(0, std::memory_order_release);
}

void MemMeld::Detach() {
  detach.store(true, std::memory_order_relaxed);
  while (pid_t tid = tracer.load(std::memory_order_acquire)) {
    if (tid == gettid()) return;
    LinuxSyscall(__NR_tgkill, getpid(), tid, WakeSignal());
    usleep(1000);
  }
}

Span<char> MemMeld::Local() const {
  return {regions.mirror, regions.high_water.load(std::memory_order_relaxed)};
}

Span<char> MemMeld::ToLocal(uintptr_t remote, Size size) const {
  if (!size || remote >= kUserEnd || size > kUserEnd - remote) return {};
  MapleTree::Segment segment = regions.by_remote.Query(remote);
  if (!segment.value || remote + size > segment.end) return {};
  return {(char*)Translate(remote, segment.value), size};
}

uintptr_t MemMeld::ToRemote(const void* local) const {
  uint64_t at = (uint64_t)local;
  if (at - (uint64_t)regions.mirror >= kMirrorReserve) return 0;
  uint64_t value = regions.by_local.Query(at).value;
  return value ? Translate(at, value) : 0;
}

const MemMeld::Region* MemMeld::Find(uintptr_t remote) const {
  if (remote >= kUserEnd) return nullptr;
  uint32_t idx = regions.IndexAt(remote);
  if (!idx) return nullptr;
  const Region& r = regions.pool[idx].r;
  return remote >= r.remote && remote < r.remote + r.local.size() ? &r : nullptr;
}

}  // namespace automat
