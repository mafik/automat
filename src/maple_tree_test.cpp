// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "maple_tree.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <iterator>
#include <map>
#include <random>
#include <thread>
#include <vector>

#include "gtest.hpp"

using namespace automat;

namespace automat {

bool operator==(const MapleTree::Segment& a, const MapleTree::Segment& b) {
  return a.begin == b.begin && a.end == b.end && a.value == b.value;
}

std::ostream& operator<<(std::ostream& os, const MapleTree::Segment& s) {
  return os << "[" << s.begin << ", " << s.end << ") " << s.value;
}

}  // namespace automat

namespace {

using Segment = MapleTree::Segment;

constexpr uint64_t kEnd = UINT64_MAX;

struct Model {
  std::map<uint64_t, std::pair<uint64_t, uint64_t>> segments;

  void Update(uint64_t begin, uint64_t end, uint64_t value) {
    if (begin == end) {
      return;
    }
    auto it = segments.lower_bound(begin);
    if (it != segments.begin() && std::prev(it)->second.first > begin) {
      --it;
    }
    std::vector<Segment> cut_parts;
    while (it != segments.end() && it->first < end) {
      Segment old = {it->first, it->second.first, it->second.second};
      it = segments.erase(it);
      if (old.begin < begin) {
        cut_parts.push_back({old.begin, begin, old.value});
      }
      if (old.end > end) {
        cut_parts.push_back({end, old.end, old.value});
      }
    }
    for (const Segment& part : cut_parts) {
      segments[part.begin] = {part.end, part.value};
    }
    if (value != 0) {
      segments[begin] = {end, value};
    }
  }

  Segment Query(uint64_t key) const {
    auto next = segments.upper_bound(key);
    uint64_t gap_begin = 0;
    if (next != segments.begin()) {
      auto prev = std::prev(next);
      if (prev->second.first > key) {
        return {prev->first, prev->second.first, prev->second.second};
      }
      gap_begin = prev->second.first;
    }
    uint64_t gap_end = next == segments.end() ? kEnd : next->first;
    return {gap_begin, gap_end, 0};
  }

  Segment QueryLowerBound(uint64_t key) const {
    auto next = segments.upper_bound(key);
    if (next != segments.begin() && std::prev(next)->second.first > key) {
      --next;
    }
    if (next == segments.end()) {
      return {0, 0, 0};
    }
    return {next->first, next->second.first, next->second.second};
  }

  std::vector<Segment> All() const {
    std::vector<Segment> all;
    for (auto& [begin, rest] : segments) {
      all.push_back({begin, rest.first, rest.second});
    }
    return all;
  }
};

std::vector<Segment> All(const MapleTree& tree) { return tree.QueryRange(0, kEnd); }

void ExpectConsistent(const MapleTree& tree) {
  EXPECT_TRUE(tree.Validate());
  EXPECT_EQ(tree.AllocatedCount(), tree.NodeCount() + tree.RetiredCount());
}

void ExpectSameLowerBound(const Segment& tree_segment, const Segment& model_segment) {
  if (model_segment.value == 0) {
    EXPECT_EQ(tree_segment.value, 0u);
    EXPECT_EQ(tree_segment.end, kEnd);
  } else {
    EXPECT_EQ(tree_segment, model_segment);
  }
}

uint64_t SegmentValue(uint64_t begin, uint64_t end) {
  return (begin * 0x9E3779B97F4A7C15ull ^ (end + 0x632BE59BD9B4E019ull) * 0xC2B2AE3D27D4EB4Full) |
         1;
}

}  // namespace

TEST(MapleTreeTest, EmptyTree) {
  MapleTree tree;
  EXPECT_EQ(tree.Query(0), (Segment{0, kEnd, 0}));
  EXPECT_EQ(tree.Query(kEnd - 1), (Segment{0, kEnd, 0}));
  EXPECT_EQ(tree.Query(12345), (Segment{0, kEnd, 0}));
  EXPECT_EQ(tree.QueryLowerBound(0).value, 0u);
  EXPECT_TRUE(tree.QueryRange(0, kEnd).empty());
  EXPECT_EQ(tree.Depth(), 1u);
  EXPECT_EQ(tree.NodeCount(), 1u);
  ExpectConsistent(tree);
}

TEST(MapleTreeTest, SingleUpdate) {
  MapleTree tree;
  tree.Update(10, 21, 7);
  EXPECT_EQ(tree.Query(10), (Segment{10, 21, 7}));
  EXPECT_EQ(tree.Query(15), (Segment{10, 21, 7}));
  EXPECT_EQ(tree.Query(20), (Segment{10, 21, 7}));
  EXPECT_EQ(tree.Query(9), (Segment{0, 10, 0}));
  EXPECT_EQ(tree.Query(21), (Segment{21, kEnd, 0}));
  ExpectConsistent(tree);
}

TEST(MapleTreeTest, EmptyRanges) {
  MapleTree tree;
  tree.Update(10, 20, 7);
  size_t allocated = tree.AllocatedCount();
  tree.Update(15, 15, 8);
  tree.Update(0, 0, 8);
  tree.Update(kEnd, kEnd, 8);
  tree.Erase(12, 12);
  EXPECT_EQ(tree.AllocatedCount(), allocated);
  EXPECT_EQ(All(tree), (std::vector<Segment>{{10, 20, 7}}));
  EXPECT_TRUE(tree.QueryRange(15, 15).empty());
  EXPECT_TRUE(tree.QueryRange(0, 0).empty());
  EXPECT_TRUE(tree.QueryRange(kEnd, kEnd).empty());
  ExpectConsistent(tree);
}

TEST(MapleTreeTest, DomainEnds) {
  MapleTree tree;
  tree.Update(0, 1, 1);
  tree.Update(kEnd - 1, kEnd, 2);
  EXPECT_EQ(tree.Query(0), (Segment{0, 1, 1}));
  EXPECT_EQ(tree.Query(kEnd - 1), (Segment{kEnd - 1, kEnd, 2}));
  EXPECT_EQ(tree.Query(1), (Segment{1, kEnd - 1, 0}));
  EXPECT_EQ(tree.QueryLowerBound(1), (Segment{kEnd - 1, kEnd, 2}));
  EXPECT_EQ(tree.QueryLowerBound(kEnd - 1), (Segment{kEnd - 1, kEnd, 2}));
  ExpectConsistent(tree);
}

TEST(MapleTreeTest, WholeDomain) {
  MapleTree tree;
  tree.Update(5, 11, 1);
  tree.Update(0, kEnd, 3);
  EXPECT_EQ(tree.Query(0), (Segment{0, kEnd, 3}));
  EXPECT_EQ(tree.Query(7), (Segment{0, kEnd, 3}));
  EXPECT_EQ(tree.Query(kEnd - 1), (Segment{0, kEnd, 3}));
  EXPECT_EQ(All(tree), (std::vector<Segment>{{0, kEnd, 3}}));
  ExpectConsistent(tree);
  tree.Erase(0, kEnd);
  EXPECT_EQ(tree.Query(7), (Segment{0, kEnd, 0}));
  ExpectConsistent(tree);
}

TEST(MapleTreeTest, SingleKeySegments) {
  MapleTree tree;
  for (uint64_t key = 100; key < 300; ++key) {
    tree.Update(key, key + 1, key);
  }
  for (uint64_t key = 100; key < 300; ++key) {
    EXPECT_EQ(tree.Query(key), (Segment{key, key + 1, key}));
  }
  EXPECT_EQ(tree.Query(99), (Segment{0, 100, 0}));
  EXPECT_EQ(tree.Query(300), (Segment{300, kEnd, 0}));
  EXPECT_EQ(All(tree).size(), 200u);
  ExpectConsistent(tree);
}

TEST(MapleTreeTest, SegmentsEndingAtTheDomainEnd) {
  MapleTree tree;
  tree.Update(kEnd - 10, kEnd, 3);
  EXPECT_EQ(tree.Query(kEnd - 1), (Segment{kEnd - 10, kEnd, 3}));
  EXPECT_EQ(tree.Query(kEnd - 11), (Segment{0, kEnd - 10, 0}));
  tree.Update(kEnd - 5, kEnd, 4);
  EXPECT_EQ(tree.Query(kEnd - 6), (Segment{kEnd - 10, kEnd - 5, 3}));
  EXPECT_EQ(tree.Query(kEnd - 1), (Segment{kEnd - 5, kEnd, 4}));
  tree.Erase(kEnd - 1, kEnd);
  EXPECT_EQ(tree.Query(kEnd - 1), (Segment{kEnd - 1, kEnd, 0}));
  EXPECT_EQ(tree.Query(kEnd - 2), (Segment{kEnd - 5, kEnd - 1, 4}));
  EXPECT_EQ(tree.QueryLowerBound(kEnd - 1).value, 0u);
  EXPECT_EQ(tree.QueryRange(kEnd - 7, kEnd),
            (std::vector<Segment>{{kEnd - 10, kEnd - 5, 3}, {kEnd - 5, kEnd - 1, 4}}));
  ExpectConsistent(tree);
}

TEST(MapleTreeTest, UpdateCases) {
  MapleTree tree;
  tree.Update(100, 200, 1);
  tree.Update(100, 200, 2);
  EXPECT_EQ(All(tree), (std::vector<Segment>{{100, 200, 2}}));

  tree.Update(120, 130, 3);
  EXPECT_EQ(All(tree), (std::vector<Segment>{{100, 120, 2}, {120, 130, 3}, {130, 200, 2}}));

  tree.Update(90, 105, 4);
  EXPECT_EQ(All(tree),
            (std::vector<Segment>{{90, 105, 4}, {105, 120, 2}, {120, 130, 3}, {130, 200, 2}}));

  tree.Update(190, 211, 5);
  EXPECT_EQ(All(tree),
            (std::vector<Segment>{
                {90, 105, 4}, {105, 120, 2}, {120, 130, 3}, {130, 190, 2}, {190, 211, 5}}));

  tree.Update(100, 196, 6);
  EXPECT_EQ(All(tree), (std::vector<Segment>{{90, 100, 4}, {100, 196, 6}, {196, 211, 5}}));

  tree.Update(211, 221, 5);
  EXPECT_EQ(All(tree),
            (std::vector<Segment>{{90, 100, 4}, {100, 196, 6}, {196, 211, 5}, {211, 221, 5}}));
  EXPECT_EQ(tree.Query(210), (Segment{196, 211, 5}));
  EXPECT_EQ(tree.Query(211), (Segment{211, 221, 5}));

  tree.Erase(300, 401);
  tree.Erase(0, 90);
  EXPECT_EQ(All(tree),
            (std::vector<Segment>{{90, 100, 4}, {100, 196, 6}, {196, 211, 5}, {211, 221, 5}}));

  tree.Erase(150, 161);
  EXPECT_EQ(All(tree),
            (std::vector<Segment>{
                {90, 100, 4}, {100, 150, 6}, {161, 196, 6}, {196, 211, 5}, {211, 221, 5}}));
  EXPECT_EQ(tree.Query(155), (Segment{150, 161, 0}));

  tree.Update(195, 216, 0);
  EXPECT_EQ(All(tree),
            (std::vector<Segment>{{90, 100, 4}, {100, 150, 6}, {161, 195, 6}, {216, 221, 5}}));
  EXPECT_EQ(tree.Query(200), (Segment{195, 216, 0}));
  ExpectConsistent(tree);
}

TEST(MapleTreeTest, GapReporting) {
  MapleTree tree;
  tree.Update(10, 20, 1);
  tree.Update(30, 40, 2);
  tree.Update(40, 50, 3);
  EXPECT_EQ(tree.Query(0), (Segment{0, 10, 0}));
  EXPECT_EQ(tree.Query(9), (Segment{0, 10, 0}));
  EXPECT_EQ(tree.Query(20), (Segment{20, 30, 0}));
  EXPECT_EQ(tree.Query(29), (Segment{20, 30, 0}));
  EXPECT_EQ(tree.Query(50), (Segment{50, kEnd, 0}));
  EXPECT_EQ(tree.Query(kEnd - 1), (Segment{50, kEnd, 0}));
  tree.Erase(30, 40);
  EXPECT_EQ(tree.Query(25), (Segment{20, 40, 0}));
  tree.Erase(10, 20);
  EXPECT_EQ(tree.Query(25), (Segment{0, 40, 0}));
  tree.Erase(40, 50);
  EXPECT_EQ(tree.Query(25), (Segment{0, kEnd, 0}));
  ExpectConsistent(tree);
}

TEST(MapleTreeTest, Traversal) {
  MapleTree tree;
  for (uint64_t i = 0; i < 1000; ++i) {
    tree.Update(i * 100 + 10, i * 100 + 60, i + 1);
  }
  Segment segment = tree.QueryLowerBound(0);
  for (uint64_t i = 0; i < 1000; ++i) {
    EXPECT_EQ(segment, (Segment{i * 100 + 10, i * 100 + 60, i + 1}));
    segment = tree.QueryLowerBound(segment.end);
  }
  EXPECT_EQ(segment.value, 0u);
  EXPECT_EQ(tree.QueryLowerBound(30), (Segment{10, 60, 1}));
  EXPECT_EQ(tree.QueryLowerBound(60), (Segment{110, 160, 2}));
  EXPECT_EQ(tree.QueryRange(50, 121), (std::vector<Segment>{{10, 60, 1}, {110, 160, 2}}));
  EXPECT_EQ(tree.QueryRange(50, 110), (std::vector<Segment>{{10, 60, 1}}));
  EXPECT_EQ(tree.QueryRange(60, 110), (std::vector<Segment>{}));
  EXPECT_EQ(tree.QueryRange(99959, kEnd), (std::vector<Segment>{{99910, 99960, 1000}}));
  EXPECT_EQ(tree.QueryRange(99960, kEnd), (std::vector<Segment>{}));
  EXPECT_EQ(All(tree).size(), 1000u);
  ExpectConsistent(tree);
}

namespace {

enum class Order { kAscending, kDescending, kRandom };

void RunDifferential(uint64_t seed, uint64_t domain, size_t operations, Order order,
                     size_t validate_every) {
  SCOPED_TRACE(testing::Message() << "seed " << seed << " domain " << domain);
  std::mt19937_64 random(seed);
  MapleTree tree;
  Model model;
  uint64_t max_length = domain / 64 + 1;
  for (size_t op = 0; op < operations; ++op) {
    uint64_t begin;
    if (order == Order::kAscending) {
      begin = domain / operations * op;
    } else if (order == Order::kDescending) {
      begin = domain / operations * (operations - 1 - op);
    } else {
      begin = random() % domain;
    }
    uint64_t length = order == Order::kRandom ? random() % max_length : domain / operations / 2;
    uint64_t end = begin + std::min(length, domain - begin);
    uint64_t value = random() % 4 == 0 ? 0 : random() % 1000 + 1;
    tree.Update(begin, end, value);
    model.Update(begin, end, value);
    if (op % 3 == 0) {
      uint64_t key = random() % domain;
      EXPECT_EQ(tree.Query(key), model.Query(key));
      ExpectSameLowerBound(tree.QueryLowerBound(key), model.QueryLowerBound(key));
    }
    if (op % validate_every == 0) {
      ExpectConsistent(tree);
      if (testing::Test::HasFailure()) {
        return;
      }
    }
  }
  ExpectConsistent(tree);
  EXPECT_EQ(All(tree), model.All());
  for (int i = 0; i < 1000; ++i) {
    uint64_t key = random() % domain;
    EXPECT_EQ(tree.Query(key), model.Query(key));
    ExpectSameLowerBound(tree.QueryLowerBound(key), model.QueryLowerBound(key));
  }
  EXPECT_EQ(tree.Query(kEnd - 1), model.Query(kEnd - 1));
}

}  // namespace

TEST(MapleTreeTest, DifferentialDense) {
  for (uint64_t seed : {1, 2, 3, 4}) {
    RunDifferential(seed, 1024, 3000, Order::kRandom, 1);
  }
}

TEST(MapleTreeTest, DifferentialSparse) {
  for (uint64_t seed : {5, 6, 7}) {
    RunDifferential(seed, uint64_t(1) << 47, 3000, Order::kRandom, 1);
    RunDifferential(seed, kEnd, 3000, Order::kRandom, 1);
  }
}

TEST(MapleTreeTest, DifferentialOrdered) {
  RunDifferential(8, 1 << 20, 3000, Order::kAscending, 1);
  RunDifferential(9, 1 << 20, 3000, Order::kDescending, 1);
}

TEST(MapleTreeTest, DifferentialBig) {
  constexpr uint64_t kRanges = 200000;
  for (Order order : {Order::kAscending, Order::kDescending, Order::kRandom}) {
    std::mt19937_64 random(10);
    MapleTree tree;
    Model model;
    std::vector<uint64_t> indices(kRanges);
    for (uint64_t i = 0; i < kRanges; ++i) {
      indices[i] = order == Order::kDescending ? kRanges - 1 - i : i;
    }
    if (order == Order::kRandom) {
      std::shuffle(indices.begin(), indices.end(), random);
    }
    for (uint64_t i = 0; i < kRanges; ++i) {
      uint64_t index = indices[i];
      uint64_t begin = index * 1000 + random() % 100;
      uint64_t end = begin + random() % 800 + 1;
      tree.Update(begin, end, index + 1);
      model.Update(begin, end, index + 1);
      if (i % 20000 == 0) {
        ExpectConsistent(tree);
      }
    }
    EXPECT_GE(tree.Depth(), 4u);
    for (int i = 0; i < 100000; ++i) {
      uint64_t begin = random() % (kRanges * 1000);
      uint64_t end = begin + random() % 5000 + 1;
      uint64_t value = random() % 2 == 0 ? 0 : random() % 1000 + 1;
      tree.Update(begin, end, value);
      model.Update(begin, end, value);
      if (i % 20000 == 0) {
        ExpectConsistent(tree);
      }
      if (i % 7 == 0) {
        uint64_t key = random() % (kRanges * 1000);
        EXPECT_EQ(tree.Query(key), model.Query(key));
        ExpectSameLowerBound(tree.QueryLowerBound(key), model.QueryLowerBound(key));
      }
    }
    ExpectConsistent(tree);
    EXPECT_EQ(All(tree), model.All());
    tree.Erase(0, kRanges * 500);
    model.Update(0, kRanges * 500, 0);
    ExpectConsistent(tree);
    EXPECT_EQ(All(tree), model.All());
    tree.Clear();
    ExpectConsistent(tree);
    EXPECT_EQ(tree.Depth(), 1u);
  }
}

TEST(MapleTreeTest, GuardDelaysReclamation) {
  MapleTree tree;
  for (uint64_t i = 0; i < 100; ++i) {
    tree.Update(i * 10, i * 10 + 5, i + 1);
  }
  tree.Reclaim();
  EXPECT_EQ(tree.RetiredCount(), 0u);
  size_t retired;
  {
    MapleTree::ReadGuard guard(tree);
    tree.Update(500, 505, 1000);
    tree.Erase(0, 100);
    retired = tree.RetiredCount();
    EXPECT_GT(retired, 0u);
    for (int i = 0; i < 4; ++i) {
      tree.TryReclaim();
    }
    EXPECT_EQ(tree.RetiredCount(), retired);
    EXPECT_EQ(guard.Query(502), (Segment{500, 505, 51}));
    EXPECT_EQ(guard.Query(0), (Segment{0, 5, 1}));
    EXPECT_EQ(tree.Query(502), (Segment{500, 505, 1000}));
    EXPECT_EQ(tree.Query(0), (Segment{0, 100, 0}));
    ExpectConsistent(tree);
  }
  tree.TryReclaim();
  tree.TryReclaim();
  EXPECT_EQ(tree.RetiredCount(), 0u);
  {
    MapleTree::ReadGuard guard(tree);
    tree.Update(600, 605, 1001);
    EXPECT_GT(tree.RetiredCount(), 0u);
  }
  tree.Reclaim();
  EXPECT_EQ(tree.RetiredCount(), 0u);
  ExpectConsistent(tree);
}

TEST(MapleTreeTest, TryReclaimAdvancesPastAGuardOnce) {
  MapleTree tree;
  MapleTree::ReadGuard guard(tree);
  EXPECT_TRUE(tree.TryReclaim());
  EXPECT_FALSE(tree.TryReclaim());
}

TEST(MapleTreeTest, ReclaimWaitsForGuard) {
  MapleTree tree;
  tree.Update(1, 3, 3);
  std::atomic<bool> started = false;
  std::atomic<bool> guard_released = false;
  std::thread writer;
  {
    MapleTree::ReadGuard guard(tree);
    tree.Update(1, 3, 4);
    writer = std::thread([&] {
      started.store(true, std::memory_order_relaxed);
      tree.Reclaim();
      EXPECT_TRUE(guard_released.load(std::memory_order_relaxed));
    });
    while (!started.load(std::memory_order_relaxed)) {
      std::this_thread::yield();
    }
    for (int i = 0; i < 1000; ++i) {
      std::this_thread::yield();
    }
    EXPECT_EQ(guard.Query(1).value, 3u);
    guard_released.store(true, std::memory_order_relaxed);
  }
  writer.join();
  EXPECT_EQ(tree.RetiredCount(), 0u);
}

TEST(MapleTreeTest, ClearAndReclaim) {
  MapleTree tree;
  for (uint64_t i = 0; i < 10000; ++i) {
    tree.Update(i * 3, i * 3 + 2, i + 1);
  }
  tree.Clear();
  tree.Reclaim();
  EXPECT_EQ(tree.NodeCount(), 1u);
  EXPECT_EQ(tree.RetiredCount(), 0u);
  EXPECT_EQ(tree.AllocatedCount(), 1u);
  EXPECT_EQ(tree.Depth(), 1u);
  EXPECT_EQ(tree.Query(5), (Segment{0, kEnd, 0}));
}

TEST(MapleTreeTest, DestructorFreesEverything) {
  MapleTree tree;
  {
    MapleTree::ReadGuard guard(tree);
    for (uint64_t i = 0; i < 10000; ++i) {
      tree.Update(i * 3, i * 3 + 2, i + 1);
    }
  }
  EXPECT_GT(tree.RetiredCount(), 0u);
  EXPECT_GT(tree.NodeCount(), 1u);
  EXPECT_EQ(tree.AllocatedCount(), tree.NodeCount() + tree.RetiredCount());
}

namespace {

void RunConcurrentReaders(int reader_count, int operations) {
  constexpr uint64_t kBlock = 64;
  constexpr uint64_t kBlocks = 1024;
  constexpr uint64_t kDomain = kBlock * kBlocks;
  constexpr uint64_t kPair = uint64_t(1) << 40;
  MapleTree tree;
  std::atomic<bool> stop = false;
  std::atomic<uint64_t> reads = 0;
  std::vector<std::thread> readers;
  for (int r = 0; r < reader_count; ++r) {
    readers.emplace_back([&, r] {
      std::mt19937_64 random(100 + r);
      uint64_t count = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        uint64_t key = random() % kDomain;
        Segment segment = tree.Query(key);
        ASSERT_LE(segment.begin, key);
        ASSERT_GT(segment.end, key);
        if (segment.value != 0) {
          ASSERT_EQ(segment.value, SegmentValue(segment.begin, segment.end));
        }
        Segment next = tree.QueryLowerBound(key);
        if (next.value != 0 && next.begin != kPair) {
          ASSERT_GT(next.end, key);
          ASSERT_EQ(next.value, SegmentValue(next.begin, next.end));
        }
        MapleTree::ReadGuard guard(tree);
        Segment a = guard.Query(kPair);
        Segment b = guard.Query(kPair + 1);
        ASSERT_EQ(a, b);
        if (count % 16 == 0) {
          uint64_t keys[64];
          Segment first_round[64];
          for (int k = 0; k < 64; ++k) {
            keys[k] = random() % kDomain;
            first_round[k] = guard.Query(keys[k]);
          }
          for (int round = 0; round < 20; ++round) {
            for (int k = 0; k < 64; ++k) {
              ASSERT_EQ(guard.Query(keys[k]), first_round[k]);
            }
          }
        }
        if (count % 64 == 0) {
          uint64_t previous_end = 0;
          for (const Segment& s : guard.QueryRange(0, kDomain)) {
            ASSERT_EQ(s.value, SegmentValue(s.begin, s.end));
            ASSERT_LT(s.begin, s.end);
            ASSERT_GE(s.begin, previous_end);
            previous_end = s.end;
          }
        }
        ++count;
      }
      reads.fetch_add(count, std::memory_order_relaxed);
    });
  }
  std::mt19937_64 random(99);
  for (int op = 0; op < operations; ++op) {
    uint64_t block = random() % kBlocks;
    int kind = random() % 1000;
    if (kind == 0) {
      tree.Clear();
    } else if (kind < 400) {
      uint64_t blocks = std::min<uint64_t>(random() % 32 + 1, kBlocks - block);
      tree.Erase(block * kBlock, (block + blocks) * kBlock);
    } else {
      uint64_t begin = block * kBlock;
      uint64_t end = begin + block % (kBlock - 3) + 1;
      tree.Update(begin, end, SegmentValue(begin, end));
    }
    if (op % 8 == 0) {
      tree.Update(kPair, kPair + 2, op + 1);
    }
    if (op % 64 == 0) {
      tree.TryReclaim();
    }
  }
  stop.store(true, std::memory_order_relaxed);
  for (std::thread& reader : readers) {
    reader.join();
  }
  EXPECT_GT(reads.load(std::memory_order_relaxed), 0u);
  tree.Reclaim();
  EXPECT_EQ(tree.RetiredCount(), 0u);
  ExpectConsistent(tree);
}

}  // namespace

TEST(MapleTreeTest, ConcurrentReaders) { RunConcurrentReaders(4, 300000); }

TEST(MapleTreeTest, ConcurrentReadersSixteenThreads) { RunConcurrentReaders(16, 100000); }

TEST(MapleTreeTest, DISABLED_Benchmark) {
  using Clock = std::chrono::steady_clock;
  for (uint64_t ranges : {10000, 1000000}) {
    MapleTree tree;
    std::mt19937_64 random(1);
    auto start = Clock::now();
    for (uint64_t i = 0; i < ranges; ++i) {
      tree.Update(i * 4096, i * 4096 + 2048, i + 1);
    }
    double update_ns = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
    constexpr int kQueries = 4000000;
    auto measure_queries = [&] {
      std::mt19937_64 reader_random(2);
      uint64_t sum = 0;
      auto begin = Clock::now();
      for (int i = 0; i < kQueries; ++i) {
        sum += tree.Query(reader_random() % (ranges * 4096)).value;
      }
      EXPECT_NE(sum, 1u);
      return std::chrono::duration<double, std::nano>(Clock::now() - begin).count() / kQueries;
    };
    double single_ns = measure_queries();
    std::atomic<bool> stop = false;
    std::thread writer([&] {
      for (uint64_t i = 0; !stop.load(std::memory_order_relaxed); ++i) {
        uint64_t begin = random() % ranges * 4096;
        tree.Update(begin, begin + 2048, i + 1);
      }
    });
    std::vector<double> parallel_ns(4);
    std::vector<std::thread> readers;
    for (int r = 0; r < 4; ++r) {
      readers.emplace_back([&, r] { parallel_ns[r] = measure_queries(); });
    }
    for (std::thread& reader : readers) {
      reader.join();
    }
    stop.store(true, std::memory_order_relaxed);
    writer.join();
    start = Clock::now();
    for (uint64_t i = 0; i < ranges; ++i) {
      tree.Erase(i * 4096, i * 4096 + 2048);
    }
    double erase_ns = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
    printf(
        "%lu segments: Update %.0f ns, Erase %.0f ns, Query %.1f ns, Query with 4 readers %.1f "
        "ns\n",
        ranges, update_ns / ranges, erase_ns / ranges, single_ns,
        (parallel_ns[0] + parallel_ns[1] + parallel_ns[2] + parallel_ns[3]) / 4);
  }
}
