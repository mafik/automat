#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace automat {

struct SegmentTree {
  // TODO: Replace with concepts
  using T = int;
  static constexpr T kDefualt = T{};
  static T Op(T a, T b) { return std::max(a, b); }

  int leaf_begin;
  // Index 0 is unused so we allocate one element less and shift the `tree`
  // pointer by one. Dereferencing `tree[0]` will segfault.
  T* tree;

  // Segment tree needs between `n * 2` (for powers of two) and `n * 3` nodes.
  SegmentTree(int n)
      : leaf_begin(1 << std::__bit_width(n - 1)), tree(new T[leaf_begin + n - 1] - 1) {
    std::fill(tree, tree + leaf_begin + n - 1, kDefualt);
  }
  ~SegmentTree() { delete[] (tree + 1); }
  void Update(int i, T val) {
    // Start at the leaf depth and move up to the root.
    int node = leaf_begin + i;
    tree[node] = val;
    while (node > 1) {
      int parent = node >> 1;
      int sibling = node ^ 1;
      tree[parent] = Op(tree[node], tree[sibling]);
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
    T result = kDefualt;
    while (true) {
      // Usually we end with both l and r pointing at the same node - we can use
      // its precomputed value and return.
      if (l == r) {
        return Op(result, tree[base + l]);
      }
      // If the l is a right child, add it to the result.
      if (l & 1) {
        result = Op(result, tree[base + l++]);  // note the "++"!
      }
      // If the r is a left child, add it to the result.
      if ((r & 1) == 0) {
        result = Op(result, tree[base + r--]);  // note the "--"!
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

}  // namespace automat
