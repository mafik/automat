#include "color.h"

namespace automaton {

SkColor SkColorFromHex(const char *hex) {
  int r, g, b;
  sscanf(hex, "#%02x%02x%02x", &r, &g, &b);
  return SkColorSetRGB(r, g, b);
}

} // namespace automaton
