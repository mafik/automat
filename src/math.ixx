module;

#define HMM_PREFIX
#include <HandmadeMath.h>

export module math;

import <algorithm>;
import <limits>;
import <ostream>;
import <iomanip>;

export using vec2 = hmm_vec2;
export using vec3 = hmm_vec3;
export using vec4 = hmm_vec4;
export using mat4 = hmm_mat4;
export using quat = hmm_quaternion;

// Project vector p onto vector dir.
export template <typename T> float VectorProjection(T dir, T p) {
  float dir2 = Dot(dir, dir);
  if (dir2 == 0)
    return 0;
  return Dot(p, dir) / dir2;
}

export template <typename T> float SegmentProjection(T a, T b, T p) {
  return VectorProjection(b - a, p - a);
}

export template <typename T> T LimitLength(T vec, float limit) {
  float len2 = LengthSquared(vec);
  if (len2 > limit * limit) {
    return vec / sqrtf(len2) * limit;
  }
  return vec;
}

export template <typename T> T ClosestPointOnSegment(T a, T b, T p) {
  return a + Saturate(SegmentProjection(a, b, p)) * (b - a);
}

export template <typename T> bool PointInRectangle(T a, T b, T c, T p) {
  float s = SegmentProjection(a, b, p);
  if ((s < 0) || (s > 1))
    return false;
  float t = SegmentProjection(a, c, p);
  return (t >= 0) && (t <= 1);
}

export std::ostream &operator<<(std::ostream &s, const vec3 &v) {
  s << std::setprecision(9) << "Vec3(" << v.X << ", " << v.Y << ", " << v.Z
    << ")";
  return s;
}

export float Saturate(float x) { return std::clamp(x, 0.f, 1.f); }

export mat4 InvertMatrix(mat4 x) {
  mat4 s = {
      x[1][1] * x[2][2] - x[2][1] * x[1][2],
      x[2][1] * x[0][2] - x[0][1] * x[2][2],
      x[0][1] * x[1][2] - x[1][1] * x[0][2],
      0,

      x[2][0] * x[1][2] - x[1][0] * x[2][2],
      x[0][0] * x[2][2] - x[2][0] * x[0][2],
      x[1][0] * x[0][2] - x[0][0] * x[1][2],
      0,

      x[1][0] * x[2][1] - x[2][0] * x[1][1],
      x[2][0] * x[0][1] - x[0][0] * x[2][1],
      x[0][0] * x[1][1] - x[1][0] * x[0][1],
      0,

      0,
      0,
      0,
      1,
  };

  auto r = x[0][0] * s[0][0] + x[0][1] * s[1][0] + x[0][2] * s[2][0];

  if (std::abs(r) >= 1) {
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) {
        s.Elements[i][j] /= r;
      }
    }
  } else {
    auto mr = std::abs(r) / std::numeric_limits<float>::min();

    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) {
        if (mr > std::abs(s[i][j])) {
          s.Elements[i][j] /= r;
        } else {
          // error
          return Mat4d(1);
        }
      }
    }
  }

  s.Elements[3][0] = -x[3][0] * s[0][0] - x[3][1] * s[1][0] - x[3][2] * s[2][0];
  s.Elements[3][1] = -x[3][0] * s[0][1] - x[3][1] * s[1][1] - x[3][2] * s[2][1];
  s.Elements[3][2] = -x[3][0] * s[0][2] - x[3][1] * s[1][2] - x[3][2] * s[2][2];
  return s;
}
