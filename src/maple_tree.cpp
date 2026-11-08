// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "maple_tree.hpp"

#include <cassert>
#include <thread>
#include <unordered_set>

namespace automat {

MapleTree::ReadGuard::ReadGuard(const MapleTree& tree) {
  size_t thread_hash = std::hash<std::thread::id>{}(std::this_thread::get_id());
  Shard& shard = tree.shards[((thread_hash * 0x9E3779B97F4A7C15ull) >> 32) % kShards];
  for (;;) {
    uint64_t epoch = tree.epoch.load(std::memory_order_relaxed);
    readers = &shard.readers[epoch & 1];
    readers->fetch_add(1, std::memory_order_seq_cst);
    if (tree.epoch.load(std::memory_order_seq_cst) == epoch) {
      break;
    }
    readers->fetch_sub(1, std::memory_order_relaxed);
  }
  root = tree.root.load(std::memory_order_acquire);
}

MapleTree::ReadGuard::~ReadGuard() { readers->fetch_sub(1, std::memory_order_release); }

MapleTree::Segment MapleTree::ReadGuard::Query(uint64_t key) const {
  assert(key < UINT64_MAX);
  const Node* node = root;
  uint64_t begin = 0;
  uint64_t end = UINT64_MAX;
  for (;;) {
    size_t i = 0;
    size_t last = node->count - 1;
    while (i < last && node->pivots[i] <= key) {
      ++i;
    }
    if (i > 0) {
      begin = node->pivots[i - 1];
    }
    if (i < last) {
      end = node->pivots[i];
    }
    if (node->leaf) {
      return {begin, end, node->values[i]};
    }
    node = reinterpret_cast<const Node*>(node->values[i]);
  }
}

MapleTree::Segment MapleTree::ReadGuard::QueryLowerBound(uint64_t key) const {
  Segment segment = Query(key);
  if (segment.value != 0 || segment.end == UINT64_MAX) {
    return segment;
  }
  return Query(segment.end);
}

std::vector<MapleTree::Segment> MapleTree::ReadGuard::QueryRange(uint64_t begin,
                                                                 uint64_t end) const {
  assert(begin <= end);
  std::vector<Segment> result;
  for (uint64_t key = begin; key < end;) {
    Segment segment = Query(key);
    if (segment.value != 0) {
      result.push_back(segment);
    }
    key = segment.end;
  }
  return result;
}

MapleTree::MapleTree() {
  root.store(new Node{.count = 1, .leaf = true}, std::memory_order_relaxed);
  allocated = 1;
}

MapleTree::~MapleTree() {
  auto delete_subtree = [](this auto& self, Node* node) -> void {
    if (!node->leaf) {
      for (size_t i = 0; i < node->count; ++i) {
        self(reinterpret_cast<Node*>(node->values[i]));
      }
    }
    delete node;
  };
  delete_subtree(root.load(std::memory_order_relaxed));
  for (Node* node : retired) {
    delete node;
  }
  for (Node* node : waiting) {
    delete node;
  }
}

MapleTree::Segment MapleTree::Query(uint64_t key) const { return ReadGuard(*this).Query(key); }

MapleTree::Segment MapleTree::QueryLowerBound(uint64_t key) const {
  return ReadGuard(*this).QueryLowerBound(key);
}

std::vector<MapleTree::Segment> MapleTree::QueryRange(uint64_t begin, uint64_t end) const {
  return ReadGuard(*this).QueryRange(begin, end);
}

void MapleTree::Update(uint64_t begin, uint64_t end, uint64_t value) noexcept {
  assert(begin <= end);
  if (begin == end) {
    return;
  }
  TryReclaim();
  Node* old_root = root.load(std::memory_order_relaxed);
  size_t height = Depth() - 1;
  Segment root_segment = {0, UINT64_MAX, reinterpret_cast<uint64_t>(old_root)};

  auto node_of = [](const Segment& segment) { return reinterpret_cast<Node*>(segment.value); };
  auto segment_of = [&](const Segment& node_segment, size_t i) -> Segment {
    Node* node = node_of(node_segment);
    return {i == 0 ? node_segment.begin : node->pivots[i - 1],
            i + 1 == node->count ? node_segment.end : node->pivots[i], node->values[i]};
  };
  auto collect = [&](this auto& self, const Segment& node_segment, size_t node_level, size_t level,
                     uint64_t lo, uint64_t hi, std::vector<Segment>& out) -> void {
    if (node_level == level) {
      out.push_back(node_segment);
      return;
    }
    for (size_t i = 0; i < node_of(node_segment)->count; ++i) {
      Segment child = segment_of(node_segment, i);
      if (child.end > lo && child.begin < hi) {
        self(child, node_level - 1, level, lo, hi, out);
      }
    }
  };

  std::vector<Segment> run;
  collect(root_segment, height, 0, begin == 0 ? 0 : begin - 1,
          end == UINT64_MAX ? UINT64_MAX : end + 1, run);
  std::vector<Segment> items;
  auto add_leaf_value = [&](uint64_t value_begin, uint64_t value_end, uint64_t leaf_value) {
    if (leaf_value == 0 && !items.empty() && items.back().value == 0) {
      items.back().end = value_end;
    } else {
      items.push_back({value_begin, value_end, leaf_value});
    }
  };
  for (const Segment& leaf : run) {
    for (size_t i = 0; i < node_of(leaf)->count; ++i) {
      Segment old = segment_of(leaf, i);
      if (old.end <= begin || old.begin >= end) {
        add_leaf_value(old.begin, old.end, old.value);
        continue;
      }
      if (old.begin < begin) {
        add_leaf_value(old.begin, begin, old.value);
      }
      if (old.begin <= begin) {
        add_leaf_value(begin, end, value);
      }
      if (old.end > end) {
        add_leaf_value(end, old.end, old.value);
      }
    }
  }
  uint64_t run_begin = run.front().begin;
  uint64_t run_end = run.back().end;

  for (size_t level = 0;; ++level) {
    while (items.size() < kMinFill && (run_begin != 0 || run_end != UINT64_MAX)) {
      bool left = run_begin != 0;
      std::vector<Segment> neighbor;
      if (left) {
        collect(root_segment, height, level, run_begin - 1, run_begin, neighbor);
      } else {
        collect(root_segment, height, level, run_end, run_end + 1, neighbor);
      }
      for (size_t i = 0; i < node_of(neighbor[0])->count; ++i) {
        items.insert(left ? items.begin() + i : items.end(), segment_of(neighbor[0], i));
      }
      if (left) {
        run.insert(run.begin(), neighbor[0]);
        run_begin = neighbor[0].begin;
      } else {
        run.push_back(neighbor[0]);
        run_end = neighbor[0].end;
      }
    }
    for (const Segment& node : run) {
      retired.push_back(node_of(node));
    }

    std::vector<Segment> built;
    if (level > 0 && items.size() == 1) {
      built = items;
    } else {
      size_t count = (items.size() + kFanout - 1) / kFanout;
      for (size_t k = 0; k < count; ++k) {
        size_t from = items.size() * k / count;
        size_t to = items.size() * (k + 1) / count;
        Node* node = new Node{.count = static_cast<uint8_t>(to - from), .leaf = level == 0};
        ++allocated;
        for (size_t j = from; j < to; ++j) {
          node->values[j - from] = items[j].value;
          if (j + 1 < to) {
            node->pivots[j - from] = items[j].end;
          }
        }
        built.push_back({items[from].begin, items[to - 1].end, reinterpret_cast<uint64_t>(node)});
      }
    }
    run.clear();
    if (level >= height) {
      if (built.size() == 1) {
        root.store(node_of(built[0]), std::memory_order_release);
        return;
      }
      items = std::move(built);
      continue;
    }
    collect(root_segment, height, level + 1, run_begin, run_end, run);
    items.clear();
    for (const Segment& parent : run) {
      for (size_t i = 0; i < node_of(parent)->count; ++i) {
        Segment child = segment_of(parent, i);
        if (child.end <= run_begin || child.begin >= run_end) {
          items.push_back(child);
        } else if (child.begin == run_begin) {
          items.insert(items.end(), built.begin(), built.end());
        }
      }
    }
    run_begin = run.front().begin;
    run_end = run.back().end;
  }
}

void MapleTree::Erase(uint64_t begin, uint64_t end) noexcept { Update(begin, end, 0); }

void MapleTree::Clear() noexcept { Update(0, UINT64_MAX, 0); }

bool MapleTree::TryReclaim() {
  uint64_t current = epoch.load(std::memory_order_relaxed);
  for (Shard& shard : shards) {
    if (shard.readers[(current + 1) & 1].load(std::memory_order_seq_cst) != 0) {
      return false;
    }
  }
  for (Node* node : waiting) {
    delete node;
  }
  allocated -= waiting.size();
  waiting.swap(retired);
  retired.clear();
  epoch.store(current + 1, std::memory_order_seq_cst);
  return true;
}

void MapleTree::Reclaim() {
  for (int round = 0; round < 2; ++round) {
    while (!TryReclaim()) {
      std::this_thread::yield();
    }
  }
}

size_t MapleTree::NodeCount() const {
  auto count_subtree = [](this auto& self, const Node* node) -> size_t {
    size_t count = 1;
    if (!node->leaf) {
      for (size_t i = 0; i < node->count; ++i) {
        count += self(reinterpret_cast<const Node*>(node->values[i]));
      }
    }
    return count;
  };
  return count_subtree(root.load(std::memory_order_relaxed));
}

size_t MapleTree::Depth() const {
  size_t depth = 1;
  for (const Node* node = root.load(std::memory_order_relaxed); !node->leaf;
       node = reinterpret_cast<const Node*>(node->values[0])) {
    ++depth;
  }
  return depth;
}

size_t MapleTree::RetiredCount() const { return retired.size() + waiting.size(); }

size_t MapleTree::AllocatedCount() const { return allocated; }

bool MapleTree::Validate() const {
  const Node* top = root.load(std::memory_order_relaxed);
  std::unordered_set<const Node*> seen;
  for (const std::vector<Node*>* list : {&retired, &waiting}) {
    for (const Node* node : *list) {
      if (!seen.insert(node).second) {
        return false;
      }
    }
  }
  bool previous_absent = false;
  auto check = [&](this auto& self, const Node* node, size_t level, uint64_t begin,
                   uint64_t end) -> bool {
    if (!seen.insert(node).second || node->leaf != (level == 0)) {
      return false;
    }
    size_t min_count = node != top ? kMinFill : node->leaf ? 1 : 2;
    if (node->count < min_count || node->count > kFanout) {
      return false;
    }
    for (size_t i = 0; i < node->count; ++i) {
      uint64_t value_begin = i == 0 ? begin : node->pivots[i - 1];
      uint64_t value_end = i + 1 == node->count ? end : node->pivots[i];
      if (value_end <= value_begin) {
        return false;
      }
      if (node->leaf) {
        if (node->values[i] == 0 && previous_absent) {
          return false;
        }
        previous_absent = node->values[i] == 0;
      } else if (!self(reinterpret_cast<const Node*>(node->values[i]), level - 1, value_begin,
                       value_end)) {
        return false;
      }
    }
    return true;
  };
  return check(top, Depth() - 1, 0, UINT64_MAX);
}

}  // namespace automat
