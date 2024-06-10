#pragma once

#include <include/core/SkColor.h>

#include "hex.hh"
#include "math.hh"
#include "template.hh"

namespace automat::color {

// Nice article about color spaces: https://ciechanow.ski/color-spaces/

SkColor SetAlpha(SkColor color, uint8_t alpha);

SkColor SetAlpha(SkColor color, float alpha_01);

SkColor AdjustLightness(SkColor color, float adjust_percent);

SkColor MixColors(SkColor zero, SkColor one, float ratio);

}  // namespace automat::color

namespace automat {

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

}  // namespace automat