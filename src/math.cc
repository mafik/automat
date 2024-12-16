// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "math.hh"

#include "format.hh"

using namespace maf;

std::string Vec2::ToStr() const { return f("Vec2(%f, %f)", x, y); }
std::string Vec3::ToStr() const { return f("Vec3(%f, %f, %f)", x, y, z); }
std::string Rect::ToStr() const {
  return f("Rect(t=%f, r=%f, b=%f, l=%f)", top, right, bottom, left);
}

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
