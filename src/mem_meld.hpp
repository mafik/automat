#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include <sys/types.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>

#include "fn.hpp"
#include "int.hpp"
#include "maple_tree.hpp"
#include "span.hpp"
#include "status.hpp"
#include "str.hpp"
#include "vec.hpp"

namespace automat {

struct MemMeld {
  struct Region {
    uintptr_t remote;
    Span<char> local;
    int prot;
    Str path;
    Size file_offset;

    Size Size() const { return local.size_bytes(); }
  };

  struct ReadGuard {
    explicit ReadGuard(const MemMeld&);
    const Region* Find(uintptr_t remote) const;
    const Region* FindLowerBound(uintptr_t remote) const;

   private:
    MapleTree::ReadGuard by_remote;
    const MemMeld& mem_meld;
  };

  ~MemMeld();

  void Attach(int target_pidfd, Status&);
  void Detach();

  Span<char> Local() const;
  Span<char> ToLocal(uintptr_t remote, Size size) const;
  uintptr_t ToRemote(const void* local) const;

  Fn<void()> on_attached;
  Fn<void(const Region&)> on_mapped;
  Fn<void(const Region& before, const Region& after)> on_changed;
  Fn<void(const Region&)> on_unmapped;
  Fn<void(int pidfd)> on_forked;

 private:
  struct RegionTable {
    struct Rec {
      Region r;
      uint64_t key = 0;
      dev_t dev = 0;
      uint64_t inode = 0;
      uint64_t End() const;
      uint64_t Delta() const;
    };

    char* const mirror;
    std::atomic<uint64_t> high_water = 0;
    Vec<std::pair<uint64_t, uint64_t>> free_list;
    Rec* const pool;  // 22 GB of MAP_NORESERVE address space
    uint32_t pool_size = 1;
    std::deque<std::pair<uint64_t, uint32_t>> dropped;
    MapleTree by_remote, by_local;

    RegionTable();
    ~RegionTable();

    uint64_t AllocLocal(uint64_t len);
    void FreeLocal(uint64_t off, uint64_t len);
    uint32_t AddRec(uint64_t remote, uint64_t len, int prot, uint64_t key, StrView path,
                    uint64_t file_offset, char* local, dev_t dev, uint64_t inode);
    void DropRec(uint32_t idx);
    uint32_t MapRegion(uint64_t remote, uint64_t len, int prot, uint64_t key, int file,
                       StrView path, uint64_t file_offset, int memfd);
    void UnmapRegion(uint32_t idx);
    uint32_t IndexAt(uint64_t remote) const;
    uint64_t NextStart(uint64_t at, uint64_t hi);
    Vec<uint32_t> Overlapping(uint64_t lo, uint64_t hi);
    std::array<uint32_t, 3> Split(uint32_t idx, uint64_t lo, uint64_t hi);
    uint32_t Protect(uint32_t idx, int prot);
    uint32_t Shrink(uint32_t idx, uint64_t keep);
    uint32_t Move(uint32_t idx, uint64_t remote);
  };

  RegionTable regions;
  std::atomic<pid_t> tracer = 0;
  std::atomic<bool> detach = false;
  bool attached = false;
};

}  // namespace automat
