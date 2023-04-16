#pragma once

#include <include/core/SkColor.h>

namespace automaton::color {

SkColor SetAlpha(SkColor color, uint8_t alpha);

SkColor SetAlpha(SkColor color, float alpha_01);

SkColor AdjustLightness(SkColor color, float adjust_percent);

} // namespace automaton::color