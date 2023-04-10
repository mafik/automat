#include "color.h"

namespace automaton {

SkColor SkColorFromHex(const char *hex) {
  int r, g, b;
  sscanf(hex, "#%02x%02x%02x", &r, &g, &b);
  return SkColorSetRGB(r, g, b);
}

SkColor SkColorBrighten(SkColor color) {
  float hsv[3];
  SkColorToHSV(color, hsv);
  hsv[2] = std::min(hsv[2] * 1.08f, 1.f);
  return SkHSVToColor(hsv);
}

SkColor SkColorDarken(SkColor color) {
  float hsv[3];
  SkColorToHSV(color, hsv);
  hsv[2] = std::max(hsv[2] * 0.92f, 0.f);
  return SkHSVToColor(hsv);
}

} // namespace automaton
