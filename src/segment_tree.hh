#include <algorithm>
#include <bit>
#include <climits>
#include <concepts>
#include <vector>

namespace automat {

template <typename Fn>
concept IsRightBetterConcept = requires { std::predicate<Fn, int, int>; };

// A tree that can answer min/max type queries.
template <IsRightBetterConcept IsRightBetterFn>
struct SegmentTree {
  unsigned n;
  unsigned leaf_begin;

  // For each node, store the index of the first leaf that contributed to it.
  std::vector<int> tree;

  IsRightBetterFn IsRightBetter;

  // Segment tree needs between `n * 2` (for powers of two) and `n * 3` nodes.
  SegmentTree(unsigned n, IsRightBetterFn IsRightBetter = {})
      : n(n), leaf_begin(std::bit_ceil(n)), tree(leaf_begin), IsRightBetter(IsRightBetter) {
    tree[0] = UINT_MAX;
    // Fill the tree nodes with their left-most leaf indices.
    // This is an alternative to filling the tree with UINT_MAX that avoids
    // guard value checks in `ChooseBetter`.
    auto leaf_depth = std::bit_width(leaf_begin);
    for (unsigned node = 1; node < leaf_begin; ++node) {
      auto node_depth = std::bit_width(node);
      auto node_offset = node - std::bit_floor(node);
      tree[node] = std::min(n - 1, node_offset << (leaf_depth - node_depth));
    }
  }

  unsigned ChooseBetter(unsigned left, unsigned right) const {
    return IsRightBetter(left, right) ? right : left;
  }

  void Update(int i) {
    // Start at the leaf depth and move up to the root.
    int parent = (i + leaf_begin) >> 1;
    int sibling = i ^ 1;
    if (sibling < n) {
      tree[parent] = ChooseBetter(i, sibling);
    } else {
      tree[parent] = i;
    }
    i = parent;
    while (i > 1) {
      parent = i >> 1;
      sibling = i ^ 1;
      tree[parent] = ChooseBetter(tree[i], tree[sibling]);
      i = parent;
    }
  }
  int Query(unsigned l, unsigned r) const {
    // Start at the leaf depth and move up to the root.
    //
    // This is faster than recursive implementation because it's stack-free and
    // can return early. Unfortunately since we're not visiting all the nodes
    // leading to the root node, some segment tree modifications are not
    // possible.
    //
    // If this ever becomes a problem it's possible to add the root-chain
    // following - by adding some loops right before the return statements.
    int best;

    if (l >= r) return l;
    bool l_is_right_child = l & 1, r_is_left_child = (r & 1) == 0;
    if (l_is_right_child && r_is_left_child) {
      best = ChooseBetter(l++, r--);
      // In some cases it's possible to arrive at a "diamond" case where l & r
      // swap places. It means we can return immediately.
      //      o
      //    /   \
      //  o      o
      // / \    / \
      //    l  r
      if (l > r) return best;
    } else if (l_is_right_child) {
      best = l++;
    } else if (r_is_left_child) {
      best = r--;
    } else {
      best = l;  // set `best` to some neutral value
    }

    // This addition converts l & r from the space of data indices to the space of tree node
    // indices. They point at tree leaves, which are always a sequence of 0..n-1 and are not
    // actually stored. The subsequent shift operation moves up the tree so that they can be
    // dereferenced.
    l += leaf_begin;
    r += leaf_begin;
    while (true) {
      // At this point l is a left child and r is a right child. We can walk up
      // the tree as long as it's the case. Trailing bit counts allow us to take
      // multiple steps at once.
      unsigned step = std::min(std::countr_zero(l), std::countr_one(r));
      l >>= step;
      r >>= step;

      if (l == r) {
        return ChooseBetter(best, tree[l]);
      }

      l_is_right_child = l & 1;
      r_is_left_child = (r & 1) == 0;
      if (l_is_right_child) {
        best = ChooseBetter(best, tree[l++]);
      }
      // If the r is a left child, add it to the result.
      if (r_is_left_child) {
        best = ChooseBetter(best, tree[r--]);
        if (l > r) return best;  // diamond case
      }
    }
  }
};

}  // namespace automat
