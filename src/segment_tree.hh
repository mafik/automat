#include <algorithm>
#include <bit>
#include <concepts>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>

namespace automat {

// Concept defining what a segment tree configuration should provide
template <typename Config>
concept SegmentTreeConfig = requires {
  typename Config::value_type;
  { Config::default_value() } -> std::same_as<typename Config::value_type>;
  {
    Config::op(std::declval<typename Config::value_type>(),
               std::declval<typename Config::value_type>())
  } -> std::same_as<typename Config::value_type>;
};

// Configuration for segment tree that finds minimum values
template <typename T>
struct SegmentTreeMinConfig {
  using value_type = T;
  static constexpr T default_value() {
    if constexpr (std::numeric_limits<T>::has_infinity) {
      return std::numeric_limits<T>::infinity();
    } else {
      return std::numeric_limits<T>::max();
    }
  }
  static T op(T a, T b) { return std::min(a, b); }
};

// Configuration for segment tree that finds maximum values
template <typename T>
struct SegmentTreeMaxConfig {
  using value_type = T;
  static constexpr T default_value() {
    if constexpr (std::numeric_limits<T>::has_infinity) {
      return -std::numeric_limits<T>::infinity();
    } else {
      return std::numeric_limits<T>::lowest();
    }
  }
  static T op(T a, T b) { return std::max(a, b); }
};

template <SegmentTreeConfig Config>
struct SegmentTree {
  using T = typename Config::value_type;

  int leaf_begin;
  // Index 0 is unused so we allocate one element less and shift the `tree`
  // pointer by one. Dereferencing `tree[0]` will segfault.
  T* tree;

  // Segment tree needs between `n * 2` (for powers of two) and `n * 3` nodes.
  SegmentTree(int n)
      : leaf_begin(1 << std::__bit_width(n - 1)), tree(new T[leaf_begin + n - 1] - 1) {
    std::fill(tree, tree + leaf_begin + n - 1, Config::default_value());
  }
  ~SegmentTree() { delete[] (tree + 1); }
  void Update(int i, T val) {
    // Start at the leaf depth and move up to the root.
    int node = leaf_begin + i;
    tree[node] = val;
    while (node > 1) {
      int parent = node >> 1;
      int sibling = node ^ 1;
      tree[parent] = Config::op(tree[node], tree[sibling]);
      node = parent;
    }
  }
  T Query(unsigned l, unsigned r) const {
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
    T result = Config::default_value();
    while (true) {
      // Usually we end with both l and r pointing at the same node - we can use
      // its precomputed value and return.
      if (l == r) {
        return Config::op(result, tree[base + l]);
      }
      // If the l is a right child, add it to the result.
      if (l & 1) {
        result = Config::op(result, tree[base + l++]);  // note the "++"!
      }
      // If the r is a left child, add it to the result.
      if ((r & 1) == 0) {
        result = Config::op(result, tree[base + r--]);  // note the "--"!
        // In some cases it's possible to arrive at a "diamond" case where l & r
        // swap places. It's actually good because we can return immediately.
        //      o
        //    /   \
        //  o      o
        // / \    / \
        //    l  r
        if (r < l) {
          return result;
        }
      }
      // At this point l is a left child and r is a right child. We can walk up
      // the tree as long as it's the case. Trailing bit counts allow us to take
      // multiple steps at once.
      int step = std::min(std::countr_zero(l), std::countr_one(r));
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
