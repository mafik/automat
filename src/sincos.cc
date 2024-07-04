#include "sincos.hh"

#include "math.hh"

namespace maf {

void SinCos::PreRotate(SkMatrix& m) const { m.preConcat(ToMatrix()); }

void SinCos::PreRotate(SkMatrix& m, Vec2 pivot) const { m.preConcat(ToMatrix(pivot)); }

SinCos SinCos::FromVec2(Vec2 v, float length) {
  if (std::isnan(length)) {
    length = Length(v);
  }
  return SinCos(v.y / length, v.x / length);
}

SkMatrix SinCos::ToMatrix() const {
  SkMatrix m;
  m.setSinCos((float)sin, (float)cos);
  return m;
}

SkMatrix SinCos::ToMatrix(Vec2 pivot) const {
  SkMatrix m;
  m.setSinCos((float)sin, (float)cos, pivot.x, pivot.y);
  return m;
}

}  // namespace maf