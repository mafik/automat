#pragma once

// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include "vec.hh"

namespace automat {

// Find the longest common subsequence of elements from a & b.
// Elements from a & b are compared for equality using provided Cmp lambda.
// Result is a sequence of indices from a that form LCS of a & b.
template <typename Cmp>
void LongestCommonSubsequence(int n_a, int n_b, Cmp cmp, Vec<int>& lcs_a) {
  // TODO: implement LCS
}

}  // namespace automat
