// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include "arcline.hh"

#include "gtest.hh"

using namespace maf;
using namespace ::testing;

class ArcLineTest : public ::testing::Test {};

TEST_F(ArcLineTest, OutsetRect) {
  ArcLine outset = ArcLine(Vec2(0, 0), 0_deg)
                       .MoveBy(1)  // bottom edge
                       .TurnBy(90_deg, 0.f)
                       .MoveBy(1)  // right edge
                       .TurnBy(90_deg, 0.f)
                       .MoveBy(1)  // top edge
                       .TurnBy(90_deg, 0.f)
                       .MoveBy(1)  // left edge
                       .TurnBy(90_deg, 0.f)
                       .Outset(0.5);

  ArcLine expected = ArcLine(Vec2(0, -0.5), 0_deg)
                         .MoveBy(1)
                         .TurnBy(90_deg, 0.5)
                         .MoveBy(1)
                         .TurnBy(90_deg, 0.5)
                         .MoveBy(1)
                         .TurnBy(90_deg, 0.5)
                         .MoveBy(1)
                         .TurnBy(90_deg, 0.5);

  ASSERT_EQ(expected.segments.size(), outset.segments.size());
  for (int i = 0; i < expected.segments.size(); i++) {
    ASSERT_EQ(expected.types[i], outset.types[i]);
    if (expected.types[i] == ArcLine::Type::Line) {
      EXPECT_THAT(outset.segments[i].line.length,
                  ::testing::FloatEq(expected.segments[i].line.length));
    } else {
      EXPECT_EQ(outset.segments[i].arc.radius, expected.segments[i].arc.radius);
      EXPECT_EQ(outset.segments[i].arc.sweep_angle, expected.segments[i].arc.sweep_angle);
    }
  }
}

// Make sure that concave edges (line/line) are properly truncated
TEST_F(ArcLineTest, OutsetInsideOutSquare) {
  // Create an "inside-out" square
  ArcLine outset = ArcLine(Vec2(0, 0), 0_deg)
                       .MoveBy(1)             // top edge
                       .TurnBy(90_deg, -0.f)  // turn right!
                       .MoveBy(1)             // right edge
                       .TurnBy(90_deg, -0.f)
                       .MoveBy(1)  // bottom edge
                       .TurnBy(90_deg, -0.f)
                       .MoveBy(1)  // left edge
                       .TurnBy(90_deg, -0.f)
                       .Outset(0.1);

  ArcLine expected = ArcLine(Vec2(0.1, -0.1), 0_deg)
                         .MoveBy(0.8)
                         .TurnBy(90_deg, -0.f)
                         .MoveBy(0.8)
                         .TurnBy(90_deg, -0.f)
                         .MoveBy(0.8)
                         .TurnBy(90_deg, -0.f)
                         .MoveBy(0.8)
                         .TurnBy(90_deg, -0.f);

  ASSERT_EQ(expected.segments.size(), outset.segments.size());
  for (int i = 0; i < expected.segments.size(); i++) {
    ASSERT_EQ(expected.types[i], outset.types[i]);
    if (expected.types[i] == ArcLine::Type::Line) {
      EXPECT_THAT(outset.segments[i].line.length,
                  ::testing::FloatEq(expected.segments[i].line.length));
    } else {
      EXPECT_EQ(outset.segments[i].arc.radius, expected.segments[i].arc.radius);
      EXPECT_EQ(outset.segments[i].arc.sweep_angle, expected.segments[i].arc.sweep_angle);
    }
  }
}
