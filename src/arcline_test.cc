// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include "arcline.hh"

#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/core/SkPixmap.h>
#include <include/core/SkStream.h>
#include <include/encode/SkWebpEncoder.h>

#include "gtest.hh"

using namespace automat;
using namespace ::testing;

#pragma comment(lib, "skia")

class ArcLineTest : public ::testing::Test {
 protected:
  void AssertArcLinesEqual(const ArcLine& expected, const ArcLine& actual, const ArcLine& base) {
    EXPECT_EQ(expected.segments.size(), actual.segments.size()) << actual.ToStr();
    if (!HasFailure()) {
      for (int i = 0; i < expected.segments.size(); i++) {
        EXPECT_EQ(expected.types[i], actual.types[i])
            << "Expected: " << expected.ToStr() << "\nActual: " << actual.ToStr();
        if (HasFailure()) {
          break;
        }
        if (expected.types[i] == ArcLine::Type::Line) {
          EXPECT_THAT(actual.segments[i].line.length,
                      ::testing::FloatEq(expected.segments[i].line.length));
        } else {
          EXPECT_THAT(actual.segments[i].arc.radius,
                      ::testing::FloatNear(expected.segments[i].arc.radius, 0.0001f));
          EXPECT_EQ(actual.segments[i].arc.sweep_angle, expected.segments[i].arc.sweep_angle);
        }
      }
    }
    if (HasFailure()) {
      SkPath expected_path = expected.ToPath();
      SkPath actual_path = actual.ToPath();
      SkPath base_path = base.ToPath();
      SkRect bounds = expected_path.getBounds();
      bounds.joinPossiblyEmptyRect(base_path.getBounds());
      SkISize canvas_size = SkISize::Make(512, 512);
      std::vector<SkPMColor> pixels(canvas_size.width() * canvas_size.height());

      auto canvas = SkCanvas::MakeRasterDirectN32(canvas_size.width(), canvas_size.height(),
                                                  pixels.data(), canvas_size.width() * 4);
      float scale = std::min(500.f / bounds.width(), 500.f / bounds.height());
      canvas->clear(SK_ColorWHITE);
      canvas->scale(1, -1);
      canvas->translate(256, -256);
      canvas->scale(scale, scale);
      canvas->translate(-bounds.centerX(), -bounds.centerY());
      SkPaint base_paint;
      base_paint.setColor(SK_ColorBLACK);
      base_paint.setStyle(SkPaint::kStroke_Style);
      base_paint.setStrokeWidth(3 / scale);
      base_paint.setAntiAlias(true);
      canvas->drawPath(base_path, base_paint);
      canvas->drawCircle(base.start, 5 / scale, base_paint);

      SkPaint expected_paint;
      expected_paint.setColor(SK_ColorRED);
      expected_paint.setStyle(SkPaint::kStroke_Style);
      expected_paint.setStrokeWidth(3 / scale);
      expected_paint.setAlphaf(0.5);
      expected_paint.setAntiAlias(true);

      canvas->drawCircle(0, 0, 5 / scale, SkPaint());

      SkPaint actual_paint;
      actual_paint.setColor(SK_ColorBLUE);
      actual_paint.setStyle(SkPaint::kStroke_Style);
      actual_paint.setStrokeWidth(3 / scale);
      actual_paint.setAlphaf(0.5);
      actual_paint.setAntiAlias(true);
      canvas->drawPath(expected_path, expected_paint);
      canvas->drawCircle(expected.start, 5 / scale, expected_paint);
      canvas->drawPath(actual_path, actual_paint);
      canvas->drawCircle(actual.start, 5 / scale, actual_paint);
      SkPixmap pixmap;
      canvas->peekPixels(&pixmap);
      SkFILEWStream stream("tmp/test.webp");
      SkWebpEncoder::Encode(&stream, pixmap, SkWebpEncoder::Options());
    }
  }
};

TEST_F(ArcLineTest, OutsetRect) {
  ArcLine base = ArcLine(Vec2(0, 0), 0_deg)
                     .MoveBy(1)  // bottom edge
                     .TurnBy(90_deg, 0.f)
                     .MoveBy(1)  // right edge
                     .TurnBy(90_deg, 0.f)
                     .MoveBy(1)  // top edge
                     .TurnBy(90_deg, 0.f)
                     .MoveBy(1)  // left edge
                     .TurnBy(90_deg, 0.f);

  ArcLine outset = ArcLine(base).Outset(0.5);

  ArcLine expected = ArcLine(Vec2(0, -0.5), 0_deg)
                         .MoveBy(1)
                         .TurnBy(90_deg, 0.5)
                         .MoveBy(1)
                         .TurnBy(90_deg, 0.5)
                         .MoveBy(1)
                         .TurnBy(90_deg, 0.5)
                         .MoveBy(1)
                         .TurnBy(90_deg, 0.5);

  AssertArcLinesEqual(expected, outset, base);
}

// Make sure that concave edges (line/line) are properly truncated
TEST_F(ArcLineTest, OutsetInsideOutSquare) {
  ArcLine base = ArcLine(Vec2(0, 0), 0_deg)
                     .MoveBy(1)               // top edge
                     .TurnConvex(-90_deg, 0)  // turn right!
                     .MoveBy(1)               // right edge
                     .TurnConvex(-90_deg, 0)
                     .MoveBy(1)  // bottom edge
                     .TurnConvex(-90_deg, 0)
                     .MoveBy(1)  // left edge
                     .TurnConvex(-90_deg, 0);
  // Create an "inside-out" square
  ArcLine outset = ArcLine(base).Outset(0.1);

  ArcLine expected = ArcLine(Vec2(-0, 0.1), 0_deg)
                         .MoveBy(1)
                         .TurnConvex(90_deg, 0.1)
                         .MoveBy(1)
                         .TurnConvex(90_deg, 0.1)
                         .MoveBy(1)
                         .TurnConvex(90_deg, 0.1)
                         .MoveBy(1)
                         .TurnConvex(90_deg, 0.1);

  AssertArcLinesEqual(expected, outset, base);
}

TEST_F(ArcLineTest, OutsetConvexArcThenLine) {
  ArcLine base = ArcLine(Vec2(0, 0), 0_deg)
                     .TurnBy(180_deg, 1)
                     .TurnBy(180_deg, -0.f)
                     .MoveBy(3)
                     .TurnBy(90_deg, 0.f)
                     .TurnBy(90_deg, 3)
                     .TurnBy(180_deg, 2.5f);
  ArcLine outset = ArcLine(base).Outset(1);

  ArcLine expected = ArcLine(Vec2(0, -1), 0_deg)
                         .TurnBy(90_deg, 2)
                         .TurnConvex(-90_deg, 0)
                         .MoveBy(1)
                         .TurnBy(90_deg, 1)
                         .TurnBy(90_deg, 4)
                         .TurnBy(180_deg, 3.5f);

  AssertArcLinesEqual(expected, outset, base);
}

TEST_F(ArcLineTest, OutsetCircle) {
  ArcLine base = ArcLine(Vec2(0, 0), 0_deg).TurnBy(180_deg, 1).TurnBy(180_deg, 1);

  ArcLine outset = ArcLine(base).Outset(1);

  ArcLine expected = ArcLine(Vec2(0, -1), 0_deg).TurnBy(180_deg, 2).TurnBy(180_deg, 2);

  AssertArcLinesEqual(expected, outset, base);
}