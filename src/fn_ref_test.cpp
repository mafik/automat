// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include "fn_ref.hpp"

#include "gtest.hpp"

using namespace automat;

static int FreeAdd(int a, int b) { return a + b; }

TEST(FnRefTest, EmptyIsFalse) {
  FnRef<int(int)> empty;
  EXPECT_FALSE(static_cast<bool>(empty));

  FnRef<int(int)> null_constructed(nullptr);
  EXPECT_FALSE(static_cast<bool>(null_constructed));
}

TEST(FnRefTest, BindsCapturingLambda) {
  int base = 10;
  auto lambda = [&](int x) { return base + x; };
  FnRef<int(int)> ref = lambda;
  EXPECT_TRUE(static_cast<bool>(ref));
  EXPECT_EQ(ref(5), 15);

  base = 100;
  EXPECT_EQ(ref(5), 105);  // Non-owning: observes the live referent.
}

TEST(FnRefTest, BindsFreeFunction) {
  FnRef<int(int, int)> ref = FreeAdd;
  EXPECT_EQ(ref(3, 4), 7);
}

TEST(FnRefTest, ForwardsArgumentsAndReturn) {
  auto concat = [](std::string a, int b) { return a + std::to_string(b); };
  FnRef<std::string(std::string, int)> ref = concat;
  EXPECT_EQ(ref("v", 42), "v42");
}

TEST(FnRefTest, Rebinding) {
  auto a = [](int x) { return x * 2; };
  auto b = [](int x) { return x * 3; };
  FnRef<int(int)> ref = a;
  EXPECT_EQ(ref(4), 8);
  ref = b;
  EXPECT_EQ(ref(4), 12);
  ref = a;
  EXPECT_EQ(ref(4), 8);
}

TEST(FnRefTest, VoidReturn) {
  int sink = 0;
  auto sink_fn = [&](int x) { sink = x; };
  FnRef<void(int)> ref = sink_fn;
  ref(99);
  EXPECT_EQ(sink, 99);
}
