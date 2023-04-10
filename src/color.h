#pragma once

#include <include/core/SkColor.h>

namespace automaton {

SkColor SkColorFromHex(const char *hex);
SkColor SkColorBrighten(SkColor);
SkColor SkColorDarken(SkColor);

} // namespace automaton