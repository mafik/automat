// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "dec64.hh"

#include <gmock/gmock-more-matchers.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "gtest.hh"

using namespace testing;
using namespace automat;

TEST(DEC64Test, MakeRaw) {
  DEC64 x;
  x = DEC64::MakeRaw(0x7FFFFFFFFFFFFF);
  EXPECT_EQ(x.GetCoefficient(), 0x7FFFFFFFFFFFFF);
  EXPECT_EQ(x.GetExponent(), 0);
  EXPECT_EQ(x.repr, 0x7FFFFFFFFFFFFF00);

  x = DEC64::MakeRaw(-0x80000000000000);
  EXPECT_EQ(x.GetCoefficient(), -0x80000000000000);
  EXPECT_EQ(x.repr, 0x8000000000000000);

  x = DEC64::MakeRaw(1, -127);
  EXPECT_EQ(x.GetExponent(), -127);

  x = DEC64::MakeRaw(1, 127);
  EXPECT_EQ(x.GetExponent(), 127);

  x = DEC64::MakeRaw(0, -128);
  EXPECT_EQ(x.GetExponent(), -128);
  EXPECT_TRUE(x.IsNaN()) << x.repr;
}

struct TestTriple {
  DEC64 a;
  DEC64 b;
  DEC64 expected_result;
};

constexpr static DEC64 kTwo = DEC64::MakeValue(2);

TEST(DEC64Test, Add) {
  TestTriple test_triples[] = {
      {DEC64::MakeValue(2), DEC64::MakeValue(2), DEC64::MakeValue(4)},
      {DEC64::MakeValue(2), DEC64::MakeValue(-2), DEC64::MakeValue(0)},
      {DEC64::MakeValue(1), DEC64_NaN, DEC64_NaN},
      {DEC64_Min, DEC64_Max, DEC64::MakeRaw(-1, 127)},
      {DEC64::MakeRaw(1, 1), DEC64::MakeRaw(1, 0), DEC64::MakeRaw(11, 0)},
      {DEC64::MakeRaw(1, 2), DEC64::MakeRaw(1, 0), DEC64::MakeRaw(101, 0)},
      {DEC64::MakeRaw(1, 16), DEC64::MakeRaw(1, 0), DEC64::MakeRaw(10000000000000001, 0)},
      {DEC64::MakeRaw(1, 17), DEC64::MakeRaw(1, 0), DEC64::MakeRaw(10000000000000000, 1)},
      {DEC64::MakeRaw(12345678909123456, 0), DEC64::MakeRaw(1, -1),
       DEC64::MakeRaw(12345678909123456, 0)},
      {DEC64::MakeRaw(1234567890912345, 0), DEC64::MakeRaw(1, -1),
       DEC64::MakeRaw(12345678909123451, -1)},
  };
  for (auto& triple : test_triples) {
    EXPECT_EQ((triple.a + triple.b).repr, triple.expected_result.repr)
        << triple.a.ToStr() << " + " << triple.b.ToStr() << " = " << (triple.a + triple.b).ToStr()
        << " != " << triple.expected_result.ToStr();
    EXPECT_EQ((triple.b + triple.a).repr, triple.expected_result.repr);
  }
}