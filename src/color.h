#pragma once

#include <include/core/SkColor.h>

namespace automaton::color {

using ARGB = SkColor;
using RGB = ARGB;

namespace {

constexpr unsigned Round(float x) { return (unsigned)(x + 0.5f); }

constexpr bool ScalarNearlyZero(SkScalar x,
                                SkScalar tolerance = SK_ScalarNearlyZero) {
  SkASSERT(tolerance >= 0);
  return (x < 0 ? -x : x) <= tolerance;
}

constexpr SkScalar ByteToScalar(U8CPU x) {
  SkASSERT(x <= 255);
  return SkIntToScalar(x) / 255;
}

constexpr SkScalar ByteDivToScalar(int numer, U8CPU denom) {
  // cast to keep the answer signed
  return SkIntToScalar(numer) / (int)denom;
}

template <typename T>
static constexpr const T &TPin(const T &x, const T &lo, const T &hi) {
  return std::max(lo, std::min(x, hi));
}

} // namespace

constexpr void RGBToHSV(U8CPU r, U8CPU g, U8CPU b, SkScalar hsv[3]) {
  SkASSERT(hsv);

  unsigned min = std::min(r, std::min(g, b));
  unsigned max = std::max(r, std::max(g, b));
  unsigned delta = max - min;

  SkScalar v = ByteToScalar(max);
  SkASSERT(v >= 0 && v <= SK_Scalar1);

  if (0 == delta) { // we're a shade of gray
    hsv[0] = 0;
    hsv[1] = 0;
    hsv[2] = v;
    return;
  }

  SkScalar s = ByteDivToScalar(delta, max);
  SkASSERT(s >= 0 && s <= SK_Scalar1);

  SkScalar h;
  if (r == max) {
    h = ByteDivToScalar(g - b, delta);
  } else if (g == max) {
    h = SkIntToScalar(2) + ByteDivToScalar(b - r, delta);
  } else { // b == max
    h = SkIntToScalar(4) + ByteDivToScalar(r - g, delta);
  }

  h *= 60;
  if (h < 0) {
    h += SkIntToScalar(360);
  }
  SkASSERT(h >= 0 && h < SkIntToScalar(360));

  hsv[0] = h;
  hsv[1] = s;
  hsv[2] = v;
}

constexpr void ColorToHSV(SkColor color, SkScalar hsv[3]) {
  RGBToHSV(SkColorGetR(color), SkColorGetG(color), SkColorGetB(color), hsv);
}

constexpr SkColor HSVToColor(U8CPU a, const SkScalar hsv[3]) {
  SkASSERT(hsv);

  SkScalar s = TPin(hsv[1], 0.0f, 1.0f);
  SkScalar v = TPin(hsv[2], 0.0f, 1.0f);

  U8CPU v_byte = Round(v * 255);

  if (ScalarNearlyZero(s)) { // shade of gray
    return SkColorSetARGB(a, v_byte, v_byte, v_byte);
  }
  SkScalar hx = (hsv[0] < 0 || hsv[0] >= SkIntToScalar(360)) ? 0 : hsv[0] / 60;
  SkScalar w = (SkScalar)((unsigned)(hx));
  SkScalar f = hx - w;

  unsigned p = Round((SK_Scalar1 - s) * v * 255);
  unsigned q = Round((SK_Scalar1 - (s * f)) * v * 255);
  unsigned t = Round((SK_Scalar1 - (s * (SK_Scalar1 - f))) * v * 255);

  unsigned r, g, b;

  SkASSERT((unsigned)(w) < 6);
  switch ((unsigned)(w)) {
  case 0:
    r = v_byte;
    g = t;
    b = p;
    break;
  case 1:
    r = q;
    g = v_byte;
    b = p;
    break;
  case 2:
    r = p;
    g = v_byte;
    b = t;
    break;
  case 3:
    r = p;
    g = q;
    b = v_byte;
    break;
  case 4:
    r = t;
    g = p;
    b = v_byte;
    break;
  default:
    r = v_byte;
    g = p;
    b = q;
    break;
  }
  return SkColorSetARGB(a, r, g, b);
}

constexpr SkColor HSVToColor(const SkScalar hsv[3]) {
  return HSVToColor(0xFF, hsv);
}

constexpr SkColor FromHex(uint32_t hex) {
  int r, g, b;
  r = (hex >> 16) & 0xff;
  g = (hex >> 8) & 0xff;
  b = (hex >> 0) & 0xff;
  return SkColorSetRGB(r, g, b);
}
constexpr SkColor Brighten(SkColor color) {
  float hsv[3];
  ColorToHSV(color, hsv);
  hsv[2] *= 1.08f;
  return HSVToColor(hsv);
}
constexpr SkColor Darken(SkColor color) {
  float hsv[3];
  ColorToHSV(color, hsv);
  hsv[2] *= 0.92f;
  return HSVToColor(hsv);
}

} // namespace automaton::color