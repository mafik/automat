// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#include "drawing.hpp"

#include <include/core/SkCanvas.h>
#include <include/core/SkColorSpace.h>
#include <include/core/SkImage.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkPicture.h>
#include <include/core/SkPictureRecorder.h>
#include <include/core/SkSamplingOptions.h>
#include <include/core/SkShader.h>
#include <include/effects/SkGradient.h>

#include "sincos.hpp"

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

void RasterPatch::DrawCached(SkCanvas& canvas, Rect bounds, SkISize raster_size,
                             FnRef<SkPath(SkCanvas&)> draw_fn, SkPaint* paint) const {
  SkRect raster_rect = SkRect::MakeWH(raster_size.width(), raster_size.height());
  if (image == nullptr) {
    SkPictureRecorder rec;
    SkCanvas* rec_canvas = rec.beginRecording(bounds);
    clip = draw_fn(*rec_canvas);
    auto picture = rec.finishRecordingAsPicture();

    sk_sp<SkColorSpace> color_space = canvas.imageInfo().refColorSpace();
    if (color_space == nullptr) {
      color_space = SkColorSpace::MakeSRGB();
    }
    SkMatrix to_raster = SkMatrix::RectToRect(bounds, raster_rect);
    image = SkImages::DeferredFromPicture(picture, raster_size, &to_raster, nullptr,
                                          SkImages::BitDepth::kU8, color_space);
  }

  constexpr bool kBicubic = true;
  static constexpr SkSamplingOptions sampling =
      kBicubic ? SkCubicResampler::CatmullRom()
               : SkSamplingOptions(SkFilterMode::kNearest, SkMipmapMode::kNearest);
  canvas.save();
  if (!clip.isEmpty()) {
    canvas.clipPath(clip);
  }
  canvas.drawImageRect(image, raster_rect, bounds, sampling, paint,
                       SkCanvas::kStrict_SrcRectConstraint);
  canvas.restore();
}

}  // namespace automat