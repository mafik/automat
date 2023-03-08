#pragma once

#define HMM_PREFIX
#include <HandmadeMath.h>
#include <algorithm>
#include <limits>
#include <ostream>
#include <iomanip>

using vec2 = hmm_vec2;
using vec3 = hmm_vec3;
using vec4 = hmm_vec4;
using mat4 = hmm_mat4;
using quat = hmm_quaternion;

// Project vector p onto vector dir.
template <typename T> float VectorProjection(T dir, T p) {
  float dir2 = Dot(dir, dir);
  if (dir2 == 0)
    return 0;
  return Dot(p, dir) / dir2;
}

template <typename T> float SegmentProjection(T a, T b, T p) {
  return VectorProjection(b - a, p - a);
}

template <typename T> T LimitLength(T vec, float limit) {
  float len2 = LengthSquared(vec);
  if (len2 > limit * limit) {
    return vec / sqrtf(len2) * limit;
  }
  return vec;
}

template <typename T> T ClosestPointOnSegment(T a, T b, T p) {
  return a + Saturate(SegmentProjection(a, b, p)) * (b - a);
}

template <typename T> bool PointInRectangle(T a, T b, T c, T p) {
  float s = SegmentProjection(a, b, p);
  if ((s < 0) || (s > 1))
    return false;
  float t = SegmentProjection(a, c, p);
  return (t >= 0) && (t <= 1);
}

std::ostream &operator<<(std::ostream &s, const vec3 &v);

float Saturate(float x);

mat4 InvertMatrix(mat4 x);
