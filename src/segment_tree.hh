#include <algorithm>
#include <bit>
#include <concepts>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>

#include "format.hh"
#include "log.hh"

namespace automat {

// Concept defining what a segment tree configuration should provide
template <typename Config>
concept SegmentTreeConfig = requires {
  typename Config::value_type;
  { Config::neutral_value() } -> std::same_as<typename Config::value_type>;
  {  // Return true if the right value is better.
    Config::test(std::declval<typename Config::value_type>(),
                 std::declval<typename Config::value_type>())
  } -> std::same_as<bool>;
};

// Configuration for segment tree that finds minimum values
template <typename T>
struct SegmentTreeMinConfig {
  using value_type = T;
  static constexpr T neutral_value() {
    if constexpr (std::numeric_limits<T>::has_infinity) {
      return std::numeric_limits<T>::infinity();
    } else {
      return std::numeric_limits<T>::max();
    }
  }
  static bool test(T a, T b) { return a > b; }
};

// Configuration for segment tree that finds maximum values
template <typename T>
struct SegmentTreeMaxConfig {
  using value_type = T;
  static constexpr T neutral_value() {
    if constexpr (std::numeric_limits<T>::has_infinity) {
      return -std::numeric_limits<T>::infinity();
    } else {
      return std::numeric_limits<T>::lowest();
    }
  }
  static bool test(T a, T b) { return a <= b; }
};

// A tree that can answer min/max type queries.
template <SegmentTreeConfig Config>
struct SegmentTree {
  using T = typename Config::value_type;

  int leaf_begin;
  // Index 0 is unused so we allocate one element less and shift the `tree`
  // pointer by one. Dereferencing `tree[0]` will segfault.
  T* tree;

  // For each node, store the index of the first leaf that contributed to it.
  int* origins;

  // Segment tree needs between `n * 2` (for powers of two) and `n * 3` nodes.
  SegmentTree(int n)
      : leaf_begin(1 << std::__bit_width(n - 1)),
        tree(new T[leaf_begin + n + 1] - 1),  // allocating one extra leaf element because it can
                                              // sometimes be accessed during query
        origins(new int[leaf_begin]) {
    std::fill(tree + 1, tree + leaf_begin + n + 2, Config::neutral_value());
    std::fill(origins, origins + leaf_begin, -1);
  }
  ~SegmentTree() {
    delete[] (tree + 1);
    delete[] (origins);
  }
  void Update(int i, T val) {
    // Start at the leaf depth and move up to the root.
    int node = leaf_begin + i;
    tree[node] = val;
    while (node > 1) {
      int parent = node >> 1;
      int sibling = node ^ 1;
      if (Config::test(tree[node], tree[sibling])) {
        tree[parent] = tree[sibling];
        origins[parent] = sibling >= leaf_begin ? sibling - leaf_begin : origins[sibling];
      } else {
        tree[parent] = tree[node];
        origins[parent] = node >= leaf_begin ? node - leaf_begin : origins[node];
      }
      node = parent;
    }
  }
  int Query(unsigned l, unsigned r) const {
    // Start at the leaf depth and move up to the root.
    //
    // This is faster than the common implementation because it's stack-free and
    // can return early. Unfortunately since we're not visiting all the nodes
    // leading to the root node, some segment tree modifications are not
    // possible.
    //
    // If this ever becomes a problem it's possible to add the root-chain
    // following - by adding some loops right before the return statements.
    // Ideally this should only happen if the configuration concept requires it.
    int base = leaf_begin;
    T result = Config::neutral_value();
    int result_origin = -1;
    while (true) {
      // Usually we end with both l and r pointing at the same node - we can use
      // its precomputed value and return.
      if (l == r) {
        if (Config::test(result, tree[base + l])) {
          result = tree[base + l];
          result_origin = (base + l >= leaf_begin ? base + l - leaf_begin : origins[base + l]);
        }
        return result_origin;
      }
      // If the l is a right child, add it to the result.
      if (l & 1) {
        // result = Config::op(result, tree[base + l++]);  // note the "++"!
        if (Config::test(result, tree[base + l])) {
          result = tree[base + l];
          result_origin = (base + l >= leaf_begin ? base + l - leaf_begin : origins[base + l]);
        }
        ++l;
      }
      // If the r is a left child, add it to the result.
      if ((r & 1) == 0) {
        // result = Config::op(result, tree[base + r--]);  // note the "--"!
        if (Config::test(result, tree[base + r])) {
          result = tree[base + r];
          result_origin = (base + r >= leaf_begin ? base + r - leaf_begin : origins[base + r]);
        }
        // In some cases it's possible to arrive at a "diamond" case where l & r
        // swap places. It's actually good because we can return immediately.
        //      o
        //    /   \
        //  o      o
        // / \    / \
        //    l  r
        if (r <= l) {
          return result_origin;
        }
        --r;
      }
      // At this point l is a left child and r is a right child. We can walk up
      // the tree as long as it's the case. Trailing bit counts allow us to take
      // multiple steps at once.
      unsigned step = std::min(std::countr_zero(l), std::countr_one(r));
      l >>= step;
      r >>= step;
      base >>= step;
    }
  }
};

template <typename T>
using SegmentTreeMin = SegmentTree<SegmentTreeMinConfig<T>>;

template <typename T>
using SegmentTreeMax = SegmentTree<SegmentTreeMaxConfig<T>>;

}  // namespace automat
