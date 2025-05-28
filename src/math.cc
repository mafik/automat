// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "math.hh"

#include "format.hh"
#include "log.hh"

using namespace automat;

std::string Vec2::ToStr() const { return f("Vec2(%f, %f)", x, y); }
std::string Vec3::ToStr() const { return f("Vec3(%f, %f, %f)", x, y, z); }
std::string Rect::ToStr() const {
  return f("Rect(t=%f, r=%f, b=%f, l=%f)", top, right, bottom, left);
}

std::string ToStrMetric(float x) { return f("%4.1fcm", x * 100); }
std::string Vec2::ToStrMetric() const { return f("(%4.1fcm, %4.1fcm)", x * 100, y * 100); }

std::string Rect::ToStrMetric() const {
  return f("Rect(t=%4.1fcm, r=%4.1fcm, b=%4.1fcm, l=%4.1fcm, w=%4.1fcm, h=%4.1fcm)", top * 100,
           right * 100, bottom * 100, left * 100, Width() * 100, Height() * 100);
}

std::string ToStrMetric(SkPoint p) { return Vec2(p).ToStrMetric(); }

std::string Vec2::ToStrPx() const { return f("%.0fx%.0fpx", roundf(x), roundf(y)); }

std::string ToStrPx(SkPoint p) { return Vec2(p).ToStrPx(); }

std::string ToStrPx(SkRect r) { return f("%gx%g%+g%+gpx", r.width(), r.height(), r.x(), r.y()); }

std::string ToStrPx(SkIRect r) { return f("%dx%d%+d%+dpx", r.width(), r.height(), r.x(), r.y()); }

void RRect::EquidistantPoints(std::span<Vec2> points) const {
  float radius = radii[0].x;
  float corners_length = 2 * M_PI * radius;
  float horiz_line_length = rect.Width() - radius * 2;
  float vert_line_length = rect.Height() - radius * 2;
  float circumference = corners_length + horiz_line_length * 2 + vert_line_length * 2;
  float step = circumference / (points.size());

  enum SegmentType {
    kTopRightCorner,
    kTopLine,
    kTopLeftCorner,
    kLeftLine,
    kBottomLeftCorner,
    kBottomLine,
    kBottomRightCorner,
    kRightLine,
  } state = kTopRightCorner;

  float segment_lengths[] = {corners_length / 4, horiz_line_length,  corners_length / 4,
                             vert_line_length,   corners_length / 4, horiz_line_length,
                             corners_length / 4, vert_line_length};

  float distance = corners_length / 8;
  for (int i = 0; i < points.size(); ++i) {
    switch (state) {
      case kTopRightCorner: {
        float angle = distance / radius;
        points[i] = Vec2(rect.right - radius + radius * cosf(angle),
                         rect.top - radius + radius * sinf(angle));
        break;
      }
      case kTopLine: {
        points[i] = Vec2(rect.right - radius - distance, rect.top);
        break;
      }
      case kTopLeftCorner: {
        float angle = M_PI_2 + (distance / radius);
        points[i] = Vec2(rect.left + radius + radius * cosf(angle),
                         rect.top - radius + radius * sinf(angle));
        break;
      }
      case kLeftLine: {
        points[i] = Vec2(rect.left, rect.top - radius - distance);
        break;
      }
      case kBottomLeftCorner: {
        float angle = M_PI + (distance / radius);
        points[i] = Vec2(rect.left + radius + radius * cosf(angle),
                         rect.bottom + radius + radius * sinf(angle));
        break;
      }
      case kBottomLine: {
        points[i] = Vec2(rect.left + radius + distance, rect.bottom);
        break;
      }
      case kBottomRightCorner: {
        float angle = M_PI + M_PI_2 + (distance / radius);
        points[i] = Vec2(rect.right - radius + radius * cosf(angle),
                         rect.bottom + radius + radius * sinf(angle));
        break;
      }
      case kRightLine: {
        // Right vertical line
        points[i] = Vec2(rect.right, rect.bottom + radius + distance);
        break;
      }
    }
    distance += step;
    while (distance >= segment_lengths[state]) {
      distance -= segment_lengths[state];
      state = (SegmentType)(((int)state + 1) % 8);
    }
  }
}