#pragma once

#include <include/core/SkPoint.h>
#include <include/core/SkRRect.h>
#include <include/core/SkRect.h>

#include <algorithm>
#include <cmath>
#include <string>

constexpr float kMetersPerInch = 0.0254f;

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
  constexpr Vec2(float x, float y) : x(x), y(y) {}
  constexpr Vec2(SkPoint p) : sk(p) {}
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
  constexpr operator SkPoint() const { return sk; }
  constexpr bool operator==(const Vec2& rhs) const { return x == rhs.x && y == rhs.y; }
  constexpr bool operator!=(const Vec2& rhs) const { return !(*this == rhs); }
  std::string ToStr() const;
};

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
};

static_assert(sizeof(Vec3) == 12, "Vec3 is not 12 bytes");

constexpr float LengthSquared(Vec2 v) { return v.x * v.x + v.y * v.y; }
inline float Length(Vec2 v) { return v.sk.length(); }

constexpr float Dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
constexpr float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

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
};

union RRect {
  SkRRect sk;
  struct {
    Rect rect;
    Vec2 radii[4];  // LL, LR, UR, UL
  };

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

  constexpr Vec2 Center() const { return rect.Center(); }
};

inline float atan(Vec2 v) { return atan2f(v.y, v.x); }

inline float NormalizeAngle(float angle) {
  while (angle < -M_PI) angle += 2 * M_PI;
  while (angle >= M_PI) angle -= 2 * M_PI;
  return angle;
}