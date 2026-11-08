#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace automat {

struct MapleTree {
  struct Segment {
    uint64_t begin, end, value;
  };

 private:
  struct Node;

 public:
  struct ReadGuard {
    explicit ReadGuard(const MapleTree& tree);
    ~ReadGuard();
    ReadGuard(const ReadGuard&) = delete;
    ReadGuard& operator=(const ReadGuard&) = delete;

    Segment Query(uint64_t key) const;
    Segment QueryLowerBound(uint64_t key) const;
    std::vector<Segment> QueryRange(uint64_t begin, uint64_t end) const;

   private:
    std::atomic<uint64_t>* readers;
    const Node* root;
  };

  MapleTree();
  ~MapleTree();
  MapleTree(const MapleTree&) = delete;
  MapleTree& operator=(const MapleTree&) = delete;

  Segment Query(uint64_t key) const;
  Segment QueryLowerBound(uint64_t key) const;
  std::vector<Segment> QueryRange(uint64_t begin, uint64_t end) const;

  void Update(uint64_t begin, uint64_t end, uint64_t value) noexcept;
  void Erase(uint64_t begin, uint64_t end) noexcept;
  void Clear() noexcept;
  bool TryReclaim();
  void Reclaim();

  size_t NodeCount() const;
  size_t Depth() const;
  size_t RetiredCount() const;
  size_t AllocatedCount() const;
  bool Validate() const;

 private:
  static constexpr size_t kFanout = 16;
  static constexpr size_t kMinFill = 8;
  static constexpr size_t kShards = 4;

  struct alignas(64) Node {
    uint8_t count;
    bool leaf;
    uint64_t pivots[kFanout - 1];
    uint64_t values[kFanout];
  };
  static_assert(sizeof(Node) == 256);

  struct alignas(64) Shard {
    std::atomic<uint64_t> readers[2];
  };

  std::atomic<Node*> root;
  std::atomic<uint64_t> epoch = 0;
  mutable Shard shards[kShards] = {};
  std::vector<Node*> retired;
  std::vector<Node*> waiting;
  size_t allocated = 0;
};

}  // namespace automat
