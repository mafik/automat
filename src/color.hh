// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <include/core/SkColor.h>
#include <include/core/SkColorFilter.h>

#include "hex.hh"
#include "math.hh"
#include "template.hh"

namespace automat::color {

// Nice article about color spaces: https://ciechanow.ski/color-spaces/

SkColor SetAlpha(SkColor color, uint8_t alpha);

SkColor SetAlpha(SkColor color, float alpha_01);

SkColor AdjustLightness(SkColor color, float adjust_percent);

SkColor MixColors(SkColor zero, SkColor one, float ratio);

constexpr SkColor FastMix(SkColor zero, SkColor one, float ratio) {
  float ratio_inv = 1.0f - ratio;
  return SkColorSetARGB(SkColorGetA(zero) * ratio_inv + SkColorGetA(one) * ratio,
                        SkColorGetR(zero) * ratio_inv + SkColorGetR(one) * ratio,
                        SkColorGetG(zero) * ratio_inv + SkColorGetG(one) * ratio,
                        SkColorGetB(zero) * ratio_inv + SkColorGetB(one) * ratio);
}

constexpr SkColor ClampedSubtractRGB(SkColor base, SkColor subtract) {
  return SkColorSetARGB(SkColorGetA(base),
                        std::max<int>(0, SkColorGetR(base) - SkColorGetR(subtract)),
                        std::max<int>(0, SkColorGetG(base) - SkColorGetG(subtract)),
                        std::max<int>(0, SkColorGetB(base) - SkColorGetB(subtract)));
}

}  // namespace automat::color

namespace automat {

maf::Str ToStr(SkColor color);

template <maf::TemplateStringArg S>
consteval SkColor operator""_color() {
  static_assert(S.c_str[0] == '#', "Color must start with #");
  static_assert(S.c_str[S.size() - 1] == '\0', "Color must end with \\0");

  if constexpr (S.size() == 8) {  // RGB
    return SkColorSetARGB(0xff, maf::HexToU8(S.c_str + 1), maf::HexToU8(S.c_str + 3),
                          maf::HexToU8(S.c_str + 5));
  } else if constexpr (S.size() == 10) {  // RGBA
    return SkColorSetARGB(maf::HexToU8(S.c_str + 7), maf::HexToU8(S.c_str + 1),
                          maf::HexToU8(S.c_str + 3), maf::HexToU8(S.c_str + 5));
  } else {
    static_assert(S.size() == 0, "Hex color must be 6 or 8 characters long");
  }
  return 0;
}

inline Vec3 SkColorToVec3(SkColor color) {
  return Vec3(SkColorGetR(color) / 255.0f, SkColorGetG(color) / 255.0f,
              SkColorGetB(color) / 255.0f);
}

inline SkColor Vec3ToSkColor(Vec3 vec) {
  return SkColorSetRGB(vec.x * 255, vec.y * 255, vec.z * 255);
}
}  // namespace automat

namespace automat::color {
constexpr SkColor kParrotRed = "#bd1929"_color;
sk_sp<SkColorFilter> MakeTintFilter(SkColor, float contrast);
sk_sp<SkColorFilter> DesaturateFilter();

}  // namespace automat::color