#pragma once

#include <include/core/SkColor.h>

namespace automaton::color {

// Nice article about color spaces: https://ciechanow.ski/color-spaces/

SkColor SetAlpha(SkColor color, uint8_t alpha);

SkColor SetAlpha(SkColor color, float alpha_01);

SkColor AdjustLightness(SkColor color, float adjust_percent);

SkColor MixColors(SkColor zero, SkColor one, float ratio);

} // namespace automaton::color
