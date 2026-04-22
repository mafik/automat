#include "drawing.hh"

#include <include/core/SkShader.h>
#include <include/effects/SkGradient.h>

#include "sincos.hh"

namespace automat {

void SetRRectShader(SkPaint& paint, const RRect& rrect, SkColor4f top, SkColor4f middle,
                    SkColor4f bottom) {
  // Get the center point of the rounded rectangle
  SkPoint center = rrect.Center();

  // Define color stops for a sweep gradient
  // We'll use strategic positions to create the transitions between colors
  constexpr int count = 8;
  SkColor4f colors[count] = {
      middle,  // right top
      top,     // top right
      top,     // top left
      middle,  // left top
      middle,  // left bottom
      bottom,  // bottom left
      bottom,  // bottom right
      middle,  // right bottom
  };

  auto Angle = [](Vec2 v) -> float { return SinCos::FromVec2(v).ToRadiansPositive() / M_PI / 2; };

  // Position stops at strategic angles (in 0-1 range where 1.0 = 360°, 0 = stright right)
  float positions[count] = {
      Angle(rrect.LineEndRightUpper()), Angle(rrect.LineEndUpperRight()),
      Angle(rrect.LineEndUpperLeft()),  Angle(rrect.LineEndLeftUpper()),
      Angle(rrect.LineEndLeftLower()),  Angle(rrect.LineEndLowerLeft()),
      Angle(rrect.LineEndLowerRight()), Angle(rrect.LineEndRightLower()),
  };

  paint.setShader(SkShaders::SweepGradient(
      center, SkGradient{SkGradient::Colors{colors, positions, SkTileMode::kClamp}, {}}));
}

}  // namespace automat