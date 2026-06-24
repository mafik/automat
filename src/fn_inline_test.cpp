// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include "fn_inline.hpp"

#include "gtest.hpp"

using namespace automat;

namespace {

int g_dtor_count = 0;

// Captured object whose destructor increments g_dtor_count exactly once per live instance.
struct DtorCounter {
  bool live = true;
  DtorCounter() = default;
  DtorCounter(const DtorCounter&) : live(true) {}
  DtorCounter(DtorCounter&& other) noexcept : live(true) { other.live = false; }
  ~DtorCounter() {
    if (live) {
      ++g_dtor_count;
    }
  }
};

}  // namespace

TEST(FnInlineTest, EmptyAndBool) {
  FnInline<int()> empty;
  EXPECT_FALSE(static_cast<bool>(empty));

  FnInline<int()> null_constructed(nullptr);
  EXPECT_FALSE(static_cast<bool>(null_constructed));

  FnInline<int()> set = [] { return 7; };
  EXPECT_TRUE(static_cast<bool>(set));
}

TEST(FnInlineTest, CallWithArgsAndReturn) {
  FnInline<int(int, int)> f = [](int a, int b) { return a * b; };
  EXPECT_EQ(f(6, 7), 42);
}

TEST(FnInlineTest, MutableCapturedState) {
  int counter = 0;
  FnInline<int()> f = [counter]() mutable { return ++counter; };
  EXPECT_EQ(f(), 1);
  EXPECT_EQ(f(), 2);
  EXPECT_EQ(f(), 3);
}

TEST(FnInlineTest, MoveLeavesSourceEmpty) {
  int value = 5;
  FnInline<int()> src = [value] { return value; };
  EXPECT_TRUE(static_cast<bool>(src));

  FnInline<int()> dst = std::move(src);
  EXPECT_FALSE(static_cast<bool>(src));
  EXPECT_TRUE(static_cast<bool>(dst));
  EXPECT_EQ(dst(), 5);
}

TEST(FnInlineTest, MoveAssignLeavesSourceEmpty) {
  FnInline<int()> src = [] { return 11; };
  FnInline<int()> dst = [] { return 22; };
  dst = std::move(src);
  EXPECT_FALSE(static_cast<bool>(src));
  EXPECT_EQ(dst(), 11);
}

TEST(FnInlineTest, CopyIsIndependent) {
  int counter = 0;
  FnInline<int()> original = [counter]() mutable { return ++counter; };
  EXPECT_EQ(original(), 1);

  FnInline<int()> copy = original;
  // Copy carries the captured counter value at copy time (1), independently advanced.
  EXPECT_EQ(copy(), 2);
  EXPECT_EQ(copy(), 3);
  // Original keeps its own counter.
  EXPECT_EQ(original(), 2);
}

TEST(FnInlineTest, DestructionRunsCapturedDtorOnce) {
  g_dtor_count = 0;
  {
    DtorCounter counter;
    FnInline<void()> f = [counter] { (void)counter.live; };
    EXPECT_EQ(g_dtor_count, 0);  // Only the temporaries so far; their dtors are suppressed by move.
  }
  // The local `counter` plus the one stored inside f: exactly two live instances destroyed.
  EXPECT_EQ(g_dtor_count, 2);
}

TEST(FnInlineTest, ResetRunsCapturedDtor) {
  g_dtor_count = 0;
  {
    DtorCounter counter;
    FnInline<void()> f = [counter] { (void)counter.live; };
    f.Reset();
    EXPECT_FALSE(static_cast<bool>(f));
    EXPECT_EQ(g_dtor_count, 1);  // The stored copy was destroyed by Reset.
  }
  EXPECT_EQ(g_dtor_count, 2);  // Plus the local `counter`.
}

TEST(FnInlineTest, ReassignmentDestroysOldCallable) {
  g_dtor_count = 0;
  {
    DtorCounter counter;
    FnInline<void()> f = [counter] { (void)counter.live; };
    EXPECT_EQ(g_dtor_count, 0);
    f = [] {};  // Replacing destroys the stored DtorCounter copy.
    EXPECT_EQ(g_dtor_count, 1);
  }
  EXPECT_EQ(g_dtor_count, 2);  // Plus the local `counter`.
}

TEST(FnInlineTest, AssignNullptrResets) {
  FnInline<int()> f = [] { return 3; };
  f = nullptr;
  EXPECT_FALSE(static_cast<bool>(f));
}
