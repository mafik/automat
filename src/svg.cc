#include "svg.h"

#include <include/utils/SkParsePath.h>

#include "log.h"

namespace automat {

SkPath PathFromSVG(const char svg[]) {
  SkPath path;
  if (!SkParsePath::FromSVGString(svg, &path)) {
    LOG() << "Failed to parse SVG path: " << svg;
  }
  constexpr float kScale = 0.0254f / 96;
  path = path.makeScale(kScale, kScale);
  path.updateBoundsCache();
  return path;
}

}  // namespace automat