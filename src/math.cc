#include "math.h"



std::ostream &operator<<(std::ostream &s, const vec3 &v) {
  s << std::setprecision(9) << "Vec3(" << v.X << ", " << v.Y << ", " << v.Z
    << ")";
  return s;
}

float Saturate(float x) { return std::clamp(x, 0.f, 1.f); }

mat4 InvertMatrix(mat4 x) {
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