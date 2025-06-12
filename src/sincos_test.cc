// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "sincos.hh"

#include <fasttrigo.h>

#include <cmath>

#include "format.hh"
#include "gtest.hh"

using namespace automat;

namespace testing {
template <>
::std::string PrintToString(const SinCos& value) {
  return f("SinCos(sin={}, cos={})", (float)value.sin, (float)value.cos);
}
}  // namespace testing

TEST(Fixed1, ConstructionZero) {
  EXPECT_EQ(Fixed1(0.0f).value, 0);
  EXPECT_EQ(Fixed1(-0.0f).value, 0);
  EXPECT_EQ(Fixed1::FromInt(0).value, 0);
}

TEST(Fixed1, ConstructionOne) {
  EXPECT_EQ(Fixed1(1.0f).value, 1 << Fixed1::N_BITS);
  EXPECT_EQ(Fixed1(-1.0f).value, -(1 << Fixed1::N_BITS));
  EXPECT_EQ(Fixed1::FromInt(1).value, 1 << Fixed1::N_BITS);
  EXPECT_EQ(Fixed1::FromInt(-1).value, -(1 << Fixed1::N_BITS));
}

TEST(Fixed1, ConstructionHalf) {
  EXPECT_EQ(Fixed1(0.5f).value, 1 << (Fixed1::N_BITS - 1));
  EXPECT_EQ(Fixed1(-0.5f).value, -(1 << (Fixed1::N_BITS - 1)));
}

TEST(Fixed1, FloatConversion) {
  EXPECT_EQ(1.f, (float)Fixed1(1.f));
  EXPECT_EQ(-1.f, (float)Fixed1(-1.f));
  EXPECT_EQ(0.5f, (float)Fixed1(0.5f));
  EXPECT_EQ(-0.5f, (float)Fixed1(-0.5f));
  EXPECT_EQ((float)M_LN2, (float)Fixed1(M_LN2));
  EXPECT_EQ(0.f, (float)Fixed1(0.f));
  EXPECT_EQ(0.f, (float)Fixed1(-0.f));
}

TEST(Fixed1, EdgeCases) {
  EXPECT_EQ(1.f, Fixed1(INFINITY));
  EXPECT_EQ(-1.f, Fixed1(-INFINITY));
  EXPECT_EQ(0.f, Fixed1(NAN));
}

TEST(Fixed1, Addition) {
  Fixed1 a(0.5f);
  Fixed1 b(0.25f);
  EXPECT_EQ(0.75f, (a + b));
  EXPECT_EQ(0.75f, (b + a));  // Commutative property
}

TEST(Fixed1, Subtraction) {
  Fixed1 a(0.5f);
  Fixed1 b(0.25f);
  EXPECT_EQ(0.25f, (a - b));
  EXPECT_EQ(-0.25f, (b - a));  // Non-commutative
}

TEST(Fixed1, Multiplication) {
  Fixed1 a(0.5f);
  Fixed1 b(2.0f);
  EXPECT_EQ(1.0f, (a * b));
  EXPECT_EQ(1.0f, (b * a));  // Commutative property
}

TEST(Fixed1, Division) {
  Fixed1 a(1.0f);
  Fixed1 b(2.0f);
  EXPECT_EQ(0.5f, (a / b));
  EXPECT_EQ(2.0f, (b / a));  // Inverse
}

TEST(Fixed1, Equality) {
  Fixed1 a(0.5f);
  Fixed1 b(0.5f);
  Fixed1 c(0.25f);
  EXPECT_TRUE(a == b);
  EXPECT_FALSE(a == c);
}

TEST(Fixed1, Inequality) {
  Fixed1 a(0.5f);
  Fixed1 b(0.5f);
  Fixed1 c(0.25f);
  EXPECT_FALSE(a != b);
  EXPECT_TRUE(a != c);
}

TEST(Fixed1, GreaterThan) {
  Fixed1 a(0.5f);
  Fixed1 b(0.25f);
  EXPECT_TRUE(a > b);
  EXPECT_FALSE(b > a);
}

TEST(Fixed1, LessThan) {
  Fixed1 a(0.5f);
  Fixed1 b(0.75f);
  EXPECT_TRUE(a < b);
  EXPECT_FALSE(b < a);
}

TEST(Fixed1, GreaterThanOrEqual) {
  Fixed1 a(0.5f);
  Fixed1 b(0.25f);
  Fixed1 c(0.5f);
  EXPECT_TRUE(a >= b);
  EXPECT_TRUE(a >= c);
  EXPECT_FALSE(b >= a);
}

TEST(Fixed1, LessThanOrEqual) {
  Fixed1 a(0.5f);
  Fixed1 b(0.75f);
  Fixed1 c(0.5f);
  EXPECT_TRUE(a <= b);
  EXPECT_TRUE(a <= c);
  EXPECT_FALSE(b <= a);
}

TEST(SinCos, Constructors) {
  {
    SinCos sc;
    EXPECT_FLOAT_EQ(0.f, (float)sc.sin);
    EXPECT_FLOAT_EQ(1.f, (float)sc.cos);
  }

  {
    SinCos sc = SinCos::FromDegrees(45.f);
    EXPECT_FLOAT_EQ(M_SQRT1_2, (float)sc.sin);
    EXPECT_FLOAT_EQ(M_SQRT1_2, (float)sc.cos);
  }

  {
    SinCos sc = SinCos::FromDegrees(30.f);
    EXPECT_NEAR(0.5f, (float)sc.sin, 0.0001f);
  }

  {
    SinCos sc = SinCos::FromDegrees(180.f);
    EXPECT_FLOAT_EQ(0.f, (float)sc.sin);
    EXPECT_FLOAT_EQ(-1.f, (float)sc.cos);
  }
}

float kTestDegrees[] = {-720.f, -360.f, -180.f, -90.f, 0.f, 30.f, 45.f, 90.f, 180.f, 360.f, 720.f};

TEST(SinCos, ToDegrees) {
  for (float degrees : kTestDegrees) {
    EXPECT_NEAR(NormalizeDegrees180(degrees), SinCos::FromDegrees(degrees).ToDegrees(), 0.001f)
        << degrees;
  }
}

TEST(SinCos, Addition) {
  for (float a : kTestDegrees) {
    for (float b : kTestDegrees) {
      EXPECT_EQ(SinCos::FromDegrees(a + b), (SinCos::FromDegrees(a) + SinCos::FromDegrees(b)))
          << a << " + " << b;
    }
  }
}

TEST(SinCos, DoubleAngle) {
  for (float a : kTestDegrees) {
    EXPECT_EQ(SinCos::FromDegrees(a * 2.f), SinCos::FromDegrees(a).DoubleAngle()) << a;
  }
}

TEST(SinCos, Scale) {
  EXPECT_EQ(270_deg .ScaleNegative(0.5f), -45_deg);
  EXPECT_EQ(270_deg .ScalePositive(0.5f), 135_deg);
  EXPECT_EQ(90_deg .ScaleNegative(0.5f), -135_deg);
  EXPECT_EQ(90_deg .ScalePositive(0.5f), 45_deg);
}

TEST(SinCos, ReflectFrom) {
  for (float ray : kTestDegrees) {
    for (float normal : kTestDegrees) {
      if (fabsf(NormalizeDegrees180(ray - normal + 180)) > 90.f) {
        // Reflection is undefined for angles greater than 90 degrees
        continue;
      }
      auto expected = SinCos::FromDegrees(2 * normal - ray - 180);
      EXPECT_EQ(expected, SinCos::FromDegrees(ray).ReflectFrom(SinCos::FromDegrees(normal)))
          << "ray " << ray << " reflected from normal " << normal << " should be "
          << expected.ToDegrees();
    }
  }
}

TEST(FastTrigo, CorrectAtan2) {
  float sin = 0.979796f, cos = -0.200000f;
  float result = FT::atan2(sin, cos);
  float ref = std::atan2(sin, cos);
  EXPECT_NEAR(result, ref, 0.01f) << "atan2(" << sin << ", " << cos << ") = " << result
                                  << " should be close to " << ref
                                  << " see "
                                     "https://github.com/Sonotsugipaa/FastTrigo/commit/"
                                     "06f9cd78f70d58aa6507706912917854ebfade0b for more info.";
}