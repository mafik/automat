// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "sincos.hh"

#include "format.hh"
#include "math.hh"

namespace maf {

void SinCos::PreRotate(SkMatrix& m) const { m.preConcat(ToMatrix()); }

void SinCos::PreRotate(SkMatrix& m, Vec2 pivot) const { m.preConcat(ToMatrix(pivot)); }

SinCos SinCos::FromVec2(Vec2 v, float length) {
  if (isnan(length)) {
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

Str SinCos::ToStr() const { return f("SinCos(%f, %f)", (float)sin, (float)cos); }
}  // namespace maf