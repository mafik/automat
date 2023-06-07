#pragma once

#include <include/core/SkPoint.h>

#include <algorithm>
#include <limits>
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
  constexpr Vec2 operator-(const Vec2& rhs) const { return Vec2(x - rhs.x, y - rhs.y); }
  constexpr Vec2 operator+(const Vec2& rhs) const { return Vec2(x + rhs.x, y + rhs.y); }
  constexpr Vec2 operator*(float rhs) const { return Vec2(x * rhs, y * rhs); }
  constexpr Vec2 operator/(float rhs) const { return Vec2(x / rhs, y / rhs); }
  constexpr operator SkPoint() const { return sk; }
  constexpr bool operator==(const Vec2& rhs) const { return x == rhs.x && y == rhs.y; }
  constexpr bool operator!=(const Vec2& rhs) const { return !(*this == rhs); }
  std::string LoggableString() const;
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
