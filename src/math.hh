// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <include/core/SkPoint.h>
#include <include/core/SkRRect.h>
#include <include/core/SkRect.h>

#include <algorithm>
#include <cmath>
#include <span>
#include <string>

#include "math_constants.hh"  // IWYU pragma: export
#include "sincos.hh"

union Vec2;

constexpr float LengthSquared(Vec2 v);

union Vec2 {
  struct {
    float x, y;
  };
  struct {
    float width, height;
  };
  float elements[2];
  SkPoint sk;

  constexpr Vec2() : x(0), y(0) {}
  constexpr Vec2(float xy) : x(xy), y(xy) {}
  constexpr Vec2(float x, float y) : x(x), y(y) {}
  constexpr Vec2(SkPoint p) : sk(p) {}
  constexpr static Vec2 Polar(maf::SinCos angle, float length) {
    return Vec2((float)angle.cos * length, (float)angle.sin * length);
  }
  constexpr static Vec2 Polar(float angle, float length) {
    return Vec2(cosf(angle) * length, sinf(angle) * length);
  }
  constexpr Vec2& operator+=(const Vec2& rhs) {
    x += rhs.x;
    y += rhs.y;
    return *this;
  }
  constexpr Vec2& operator-=(const Vec2& rhs) {
    x -= rhs.x;
    y -= rhs.y;
    return *this;
  }
  constexpr Vec2& operator*=(float rhs) {
    x *= rhs;
    y *= rhs;
    return *this;
  }
  constexpr Vec2 operator-(const Vec2& rhs) const { return Vec2(x - rhs.x, y - rhs.y); }
  constexpr Vec2 operator-() const { return Vec2(-x, -y); }
  constexpr Vec2 operator+(const Vec2& rhs) const { return Vec2(x + rhs.x, y + rhs.y); }
  constexpr Vec2 operator*(float rhs) const { return Vec2(x * rhs, y * rhs); }
  constexpr Vec2 operator/(float rhs) const { return Vec2(x / rhs, y / rhs); }
  constexpr Vec2 operator*(Vec2 other) const { return Vec2(x * other.x, y * other.y); }
  constexpr Vec2 operator/(Vec2 other) const { return Vec2(x / other.x, y / other.y); }
  constexpr operator SkPoint() const { return sk; }
  constexpr bool operator==(const Vec2& rhs) const { return x == rhs.x && y == rhs.y; }
  constexpr bool operator!=(const Vec2& rhs) const { return !(*this == rhs); }
  std::string ToStr() const;
  std::string ToStrMetric() const;
  std::string ToStrPx() const;
};

static constexpr Vec2 kZeroVec2 = {0, 0};

static_assert(sizeof(Vec2) == 8, "Vec2 is not 8 bytes");

union Vec3 {
  struct {
    float x, y, z;
  };
  struct {
    float r, g, b;
  };
  float elements[3];
  constexpr Vec3() : x(0), y(0), z(0) {}
  constexpr Vec3(float x, float y, float z) : x(x), y(y), z(z) {}
  constexpr Vec3(Vec2 xy, float z) : x(xy.x), y(xy.y), z(z) {}
  constexpr Vec3& operator+=(const Vec3& rhs) {
    x += rhs.x;
    y += rhs.y;
    z += rhs.z;
    return *this;
  }
  constexpr Vec3& operator-=(const Vec3& rhs) {
    x -= rhs.x;
    y -= rhs.y;
    z -= rhs.z;
    return *this;
  }
  constexpr Vec3 operator-(const Vec3& rhs) const { return Vec3(x - rhs.x, y - rhs.y, z - rhs.z); }
  constexpr Vec3 operator+(const Vec3& rhs) const { return Vec3(x + rhs.x, y + rhs.y, z + rhs.z); }
  constexpr Vec3 operator*(float rhs) const { return Vec3(x * rhs, y * rhs, z * rhs); }
  constexpr Vec3 operator/(float rhs) const { return Vec3(x / rhs, y / rhs, z / rhs); }
  constexpr bool operator==(const Vec3& rhs) const {
    return x == rhs.x && y == rhs.y && z == rhs.z;
  }
  constexpr bool operator!=(const Vec3& rhs) const { return !(*this == rhs); }
  std::string ToStr() const;
};

static_assert(sizeof(Vec3) == 12, "Vec3 is not 12 bytes");

constexpr float LengthSquared(Vec2 v) { return v.x * v.x + v.y * v.y; }
inline float Length(Vec2 v) { return v.sk.length(); }
constexpr float LengthSquared(Vec3 v) { return v.x * v.x + v.y * v.y + v.z * v.z; }
inline float Length(Vec3 v) { return sqrtf(LengthSquared(v)); }

inline Vec2 Normalize(Vec2 v) {
  float len = Length(v);
  if (len == 0) return {0, 0};
  return v / len;
}

inline Vec2 Round(Vec2 v) { return {roundf(v.x), roundf(v.y)}; }

inline Vec2 Rotate90DegreesClockwise(Vec2 v) { return {v.y, -v.x}; }
inline Vec2 Rotate90DegreesCounterClockwise(Vec2 v) { return {-v.y, v.x}; }

constexpr float Dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
constexpr float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
constexpr float Cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
constexpr Vec3 Cross(Vec3 a, Vec3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
template <float T>
constexpr Vec2 EvalBezierAtFixedT(Vec2 p0, Vec2 p1, Vec2 p2) {
  return p0 * (1 - T) * (1 - T) + p1 * 2 * T * (1 - T) + p2 * T * T;
}
template <float T>
constexpr Vec2 EvalBezierAtFixedT(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3) {
  constexpr float t = T;
  return p0 * (1 - t) * (1 - t) * (1 - t) + p1 * 3 * t * (1 - t) * (1 - t) +
         p2 * 3 * t * t * (1 - t) + p3 * t * t * t;
}

// Project vector p onto vector dir.
template <typename T>
float VectorProjection(T dir, T p) {
  float dir2 = Dot(dir, dir);
  if (dir2 == 0) return 0;
  return Dot(p, dir) / dir2;
}

template <typename T>
float SegmentProjection(T a, T b, T p) {
  return VectorProjection(b - a, p - a);
}

template <typename T>
T LimitLength(T vec, float limit) {
  float len2 = LengthSquared(vec);
  if (len2 > limit * limit) {
    return vec / sqrtf(len2) * limit;
  }
  return vec;
}

template <typename T>
T ClosestPointOnSegment(T a, T b, T p) {
  return a + Saturate(SegmentProjection(a, b, p)) * (b - a);
}

template <typename T>
bool PointInRectangle(T a, T b, T c, T p) {
  float s = SegmentProjection(a, b, p);
  if ((s < 0) || (s > 1)) return false;
  float t = SegmentProjection(a, c, p);
  return (t >= 0) && (t <= 1);
}

inline float Saturate(float x) { return std::clamp(x, 0.f, 1.f); }

// Return value between 0 and 1, based on the value and the range.
inline float GetRatio(float val, float min, float max) {
  return Saturate((val - min) / (max - min));
}

// A smooth ReLU function.
inline float SoftPlus(float x, float beta = 1) { return logf(1 + expf(beta * x)) / beta; }

template <typename T>
T ClampLength(T vec, float min, float max) {
  float len2 = LengthSquared(vec);
  if (len2 < min * min) {
    return vec * (min / sqrtf(len2));
  }
  if (len2 > max * max) {
    return vec * (max / sqrtf(len2));
  }
  return vec;
}

struct CenterX;
struct CenterY;

// Helper for working in the Skia coordinate system. Swaps the coordinates on the Y axis.
//
// Skia uses a coordinate system where the Y axis points down.
//
// Automat uses a coordinate system where the Y axis points up.
//
// This utility class allows one to alias the SkRect and use the `top` & `bottom` fields to access
// the proper SkRect fields (`fBottom` & `fTop` respectively).
union Rect {
  SkRect sk;
  struct {
    // Smaller x-axis bound.
    float left = 0;
    // Smaller y-axis bound.
    float bottom = 0;
    // Larger x-axis bound.
    float right = 0;
    // Larger y-axis bound.
    float top = 0;
  };

  constexpr Rect() = default;
  constexpr Rect(SkRect r) : sk(r) {}
  constexpr Rect(float left, float bottom, float right, float top)
      : left(left), bottom(bottom), right(right), top(top) {}

  template <typename AnchorX = ::CenterX, typename AnchorY = ::CenterY>
  static constexpr Rect MakeAtZero(float width, float height) {
    Rect r{0, 0, width, height};
    return r.MoveBy({-AnchorX{}(r), -AnchorY{}(r)});
  }

  template <typename AnchorX = ::CenterX, typename AnchorY = ::CenterY>
  static constexpr Rect MakeAtZero(Vec2 size) {
    return MakeAtZero<AnchorX, AnchorY>(size.x, size.y);
  }

  // Construct a zero-sized rectangle at the given point.
  static constexpr Rect MakeEmptyAt(Vec2 point) { return {point.x, point.y, point.x, point.y}; }

  // Make a rectangle with the lower left corner at (0,0) and given width & height.
  static constexpr Rect MakeCornerZero(float width, float height) { return {0, 0, width, height}; }

  // Make a rectangle with the center at (0, 0) and given width & height.
  static constexpr Rect MakeCenterZero(float width, float height) {
    return {-width / 2, -height / 2, width / 2, height / 2};
  }
  static constexpr Rect MakeCenter(Vec2 center, float width, float height) {
    return {center.x - width / 2, center.y - height / 2, center.x + width / 2,
            center.y + height / 2};
  }
  static constexpr Rect MakeCircleR(float r) { return {-r, -r, r, r}; }

  operator SkRect&() { return sk; }

  static constexpr float MinY(const SkRect& r) { return r.fTop; }
  static constexpr float MaxY(const SkRect& r) { return r.fBottom; }
  static constexpr float MinX(const SkRect& r) { return r.fLeft; }
  static constexpr float MaxX(const SkRect& r) { return r.fRight; }
  constexpr float MinY() const { return bottom; }
  constexpr float MaxY() const { return top; }
  constexpr float MinX() const { return left; }
  constexpr float MaxX() const { return right; }

  static constexpr float CenterY(const SkRect& r) { return (r.fTop + r.fBottom) / 2; }
  static constexpr float CenterX(const SkRect& r) { return (r.fLeft + r.fRight) / 2; }
  constexpr float CenterY() const { return (top + bottom) / 2; }
  constexpr float CenterX() const { return (left + right) / 2; }

  // Note that the Skia coordinate system has the Y axis pointing down!
  static constexpr float Width(const SkRect& r) { return r.fRight - r.fLeft; }
  static constexpr float Height(const SkRect& r) { return r.fBottom - r.fTop; }
  constexpr float Width() const { return right - left; }
  constexpr float Height() const { return top - bottom; }

  static constexpr Vec2 Size(const SkRect& r) { return {Width(r), Height(r)}; }
  constexpr Vec2 Size() const { return {Width(), Height()}; }

  static constexpr Vec2 Center(const SkRect& r) { return r.center(); }
  constexpr Vec2 Center() const { return sk.center(); }

  static constexpr Vec2 TopLeftCorner(const SkRect& r) { return {r.fLeft, r.fBottom}; }
  static constexpr Vec2 TopCenter(const SkRect& r) { return {CenterX(r), r.fBottom}; }
  static constexpr Vec2 TopRightCorner(const SkRect& r) { return {r.fRight, r.fBottom}; }
  static constexpr Vec2 BottomLeftCorner(const SkRect& r) { return {r.fLeft, r.fTop}; }
  static constexpr Vec2 BottomCenter(const SkRect& r) { return {CenterX(r), r.fTop}; }
  static constexpr Vec2 BottomRightCorner(const SkRect& r) { return {r.fRight, r.fTop}; }
  static constexpr Vec2 LeftCenter(const SkRect& r) { return {r.fLeft, CenterY(r)}; }
  static constexpr Vec2 RightCenter(const SkRect& r) { return {r.fRight, CenterY(r)}; }
  constexpr Vec2 TopLeftCorner() const { return {left, top}; }
  constexpr Vec2 TopCenter() const { return {CenterX(), top}; }
  constexpr Vec2 TopRightCorner() const { return {right, top}; }
  constexpr Vec2 BottomLeftCorner() const { return {left, bottom}; }
  constexpr Vec2 BottomCenter() const { return {CenterX(), bottom}; }
  constexpr Vec2 BottomRightCorner() const { return {right, bottom}; }
  constexpr Vec2 LeftCenter() const { return {left, CenterY()}; }
  constexpr Vec2 RightCenter() const { return {right, CenterY()}; }

  constexpr float DistanceSquared(Vec2 point) {
    float dx = std::max(0.f, std::max(left - point.x, point.x - right));
    float dy = std::max(0.f, std::max(bottom - point.y, point.y - top));
    return dx * dx + dy * dy;
  }
  constexpr float Discance(Vec2 point) { return sqrtf(DistanceSquared(point)); }

  constexpr bool Contains(const Rect& other) const {
    return left <= other.left && right >= other.right && bottom <= other.bottom && top >= other.top;
  }

  constexpr bool Contains(Vec2 point) const {
    return point.x >= left && point.x <= right && point.y >= bottom && point.y <= top;
  }

  void ExpandToInclude(Vec2 point) {
    left = std::min(left, point.x);
    right = std::max(right, point.x);
    bottom = std::min(bottom, point.y);
    top = std::max(top, point.y);
  }

  void ExpandToInclude(const Rect& other) {
    ExpandToInclude(other.TopLeftCorner());
    ExpandToInclude(other.BottomRightCorner());
  }

  [[nodiscard]] constexpr Rect Outset(float amount) const {
    return {left - amount, bottom - amount, right + amount, top + amount};
  }

  [[nodiscard]] constexpr Rect MoveBy(Vec2 offset) const {
    return {left + offset.x, bottom + offset.y, right + offset.x, top + offset.y};
  }

  std::string ToStr() const;
  std::string ToStrMetric() const;
};

struct LeftX {
  constexpr float operator()(const Rect& r) const { return r.left; }
};
struct CenterX {
  constexpr float operator()(const Rect& r) const { return r.CenterX(); }
};
struct RightX {
  constexpr float operator()(const Rect& r) const { return r.right; }
};
struct TopY {
  constexpr float operator()(const Rect& r) const { return r.top; }
};
struct CenterY {
  constexpr float operator()(const Rect& r) const { return r.CenterY(); }
};
struct BottomY {
  constexpr float operator()(const Rect& r) const { return r.bottom; }
};

std::string ToStrMetric(float);
std::string ToStrMetric(SkPoint);
std::string ToStrPx(SkPoint);
std::string ToStrPx(SkRect);
std::string ToStrPx(SkIRect);

union RRect {
  SkRRect sk;
  struct {
    Rect rect;
    Vec2 radii[4];  // LowerLeft, LowerRight, UpperRight, UpperLeft
    SkRRect::Type type;
  };

  // Make an RRect with non-zero width and height with equal radii.
  constexpr static RRect MakeSimple(Rect rect, float radius) {
    return {
        .rect = rect, .radii{radius, radius, radius, radius}, .type = SkRRect::Type::kSimple_Type};
  }

  [[nodiscard]] constexpr RRect Outset(float amount) const {
    auto AdjustRadius = [amount](Vec2 r) {
      return Vec2(std::max(0.f, r.x + amount), std::max(0.f, r.y + amount));
    };
    RRect ret = {.rect = rect.Outset(amount),
                 .radii =
                     {
                         AdjustRadius(radii[0]),
                         AdjustRadius(radii[1]),
                         AdjustRadius(radii[2]),
                         AdjustRadius(radii[3]),
                     },
                 .type = type};
    if (ret.rect.Width() == 0 && ret.rect.Height() == 0) {
      ret.type = SkRRect::Type::kEmpty_Type;
    } else if (ret.radii[0] == ret.radii[1] && ret.radii[1] == ret.radii[2] &&
               ret.radii[2] == ret.radii[3]) {
      ret.type = SkRRect::Type::kSimple_Type;
    } else if (ret.radii[0] == ret.radii[1] && ret.radii[1] == ret.radii[2] &&
               ret.radii[2] == ret.radii[3] && (ret.radii[0].x >= (ret.rect.Width() / 2)) &&
               (ret.radii[0].y >= (ret.rect.Height() / 2))) {
      ret.type = SkRRect::Type::kOval_Type;
    } else if (ret.radii[0].y == ret.radii[1].y && ret.radii[2].y == ret.radii[3].y &&
               ret.radii[0].x == ret.radii[3].x && ret.radii[1].x == ret.radii[2].x) {
      ret.type = SkRRect::Type::kNinePatch_Type;
    } else {
      ret.type = SkRRect::Type::kComplex_Type;
    }
    return ret;
  }

  // Left end of the upper line.
  Vec2 LineEndUpperLeft() const { return {rect.left + radii[3].x, rect.top}; }
  // Right end of the upper line.
  Vec2 LineEndUpperRight() const { return {rect.right - radii[2].x, rect.top}; }
  // Left end of the lower line.
  Vec2 LineEndLowerLeft() const { return {rect.left + radii[0].x, rect.bottom}; }
  // Right end of the lower line.
  Vec2 LineEndLowerRight() const { return {rect.right - radii[1].x, rect.bottom}; }
  // Upper end of the left line.
  Vec2 LineEndLeftUpper() const { return {rect.left, rect.top - radii[3].y}; }
  // Lower end of the left line.
  Vec2 LineEndLeftLower() const { return {rect.left, rect.bottom + radii[0].y}; }
  // Upper end of the right line.
  Vec2 LineEndRightUpper() const { return {rect.right, rect.top - radii[2].y}; }
  // Lower end of the right line.
  Vec2 LineEndRightLower() const { return {rect.right, rect.bottom + radii[1].y}; }

  // At the moment only supports simple RRects.
  void EquidistantPoints(std::span<Vec2> points) const;

  constexpr Vec2 Center() const { return rect.Center(); }

  constexpr RRect Outset(float amount) {
    return {.rect = rect.Outset(amount),
            .radii = {radii[0] + Vec2(amount, amount), radii[1] + Vec2(amount, amount),
                      radii[2] + Vec2(amount, amount), radii[3] + Vec2(amount, amount)},
            .type = type};
  }

  constexpr RRect MoveBy(Vec2 offset) {
    return {.rect = rect.MoveBy(offset),
            .radii = {radii[0], radii[1], radii[2], radii[3]},
            .type = type};
  }
};

struct Vec2AndDir {
  Vec2 pos;
  maf::SinCos dir;
};

inline float atan(Vec2 v) { return atan2f(v.y, v.x); }

inline float CosineInterpolate(float a, float b, float t) {
  float t2 = (1 - cosf(std::clamp<float>(t, 0, 1) * M_PI)) / 2;
  return a * (1 - t2) + b * t2;
}

double constexpr Sqrt(double x) {
  auto SqrtNewtonRaphson = [](this auto& self, double x, double curr, double prev) -> double {
    return curr == prev ? curr : self(x, 0.5 * (curr + x / curr), curr);
  };
  return x >= 0 && x < std::numeric_limits<double>::infinity()
             ? SqrtNewtonRaphson(x, x, 0)
             : std::numeric_limits<double>::quiet_NaN();
}