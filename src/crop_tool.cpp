// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot
#include "crop_tool.hpp"

#include <include/core/SkBlurTypes.h>
#include <include/core/SkCanvas.h>
#include <include/core/SkColorFilter.h>
#include <include/core/SkColorSpace.h>
#include <include/core/SkImage.h>
#include <include/core/SkM44.h>
#include <include/core/SkMaskFilter.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPath.h>
#include <include/core/SkPathBuilder.h>
#include <include/core/SkPathMeasure.h>
#include <include/core/SkPicture.h>
#include <include/core/SkPictureRecorder.h>
#include <include/core/SkRSXform.h>
#include <include/core/SkShader.h>
#include <include/effects/SkDashPathEffect.h>
#include <include/effects/SkGradient.h>
#include <include/utils/SkParsePath.h>

#include <cmath>

#include "../build/generated/embedded.hpp"
#include "action.hpp"
#include "arcline.hpp"
#include "board.hpp"
#include "color.hpp"
#include "drawing.hpp"
#include "fn_ref.hpp"
#include "location.hpp"
#include "math.hpp"
#include "random.hpp"
#include "sincos.hpp"
#include "textures.hpp"
#include "units.hpp"

namespace automat {

constexpr float kHomeSide = 21_mm;
constexpr float kViewMargin = 4.2_mm;
constexpr float kViewBoundsOutset = 1_mm;
constexpr float kScissorsExtent = 16_mm;
constexpr float kElevation = 1_mm;
constexpr float kHeldElevation = 5_mm;
constexpr float kCroppedElevation = 0.3_mm;
constexpr float kPivotPeriod = 0.6;
constexpr float kTurnPeriod = 0.6;
constexpr float kScalePeriod = 0.6;
constexpr float kOpeningPeriod = 0.3;
constexpr float kUnfoldingPeriod = 0.4;
constexpr float kBoxPeriod = 0.6;
constexpr float kHalvesDistancePeriod = 0.4;
constexpr float kElevationPeriod = 0.6;
constexpr uint32_t kPivotAnchorId = 1;
constexpr float kPivotAnchorRadius = 2_cm;
constexpr float kPivotAnchorDecay = 2_cm;

struct CropTone {
  float saturation;
  float value;
};

constexpr CropTone kGripTone = {0.73f, 0.40f};
constexpr CropTone kGripShadeTone = {0.725f, 0.314f};
constexpr CropTone kGripLightTone = {0.70f, 0.51f};
constexpr CropTone kDarkTone = {0.725f, 0.157f};
constexpr CropTone kFabricTone = {0.119f, 0.957f};
constexpr CropTone kCropHandleLightTone = {0.6f, 0.55f};

static SkColor4f CropColor(float hue, CropTone tone) {
  float hsv[3] = {hue, tone.saturation, tone.value};
  return SkColor4f::FromColor(SkHSVToColor(hsv));
}

static const SinCos kScissorsLight = 135_deg;
static const SinCos kScissorsOpenAngle = 30_deg;
constexpr float kScissorsPivotRadius = 0.81_mm;
constexpr Vec2 kScissorsHomePivot = {-1.86_mm, 1.71_mm};
static const SinCos kScissorsHomeTurn = -46.6_deg;
constexpr float kScissorsPatchDensity = 12 / 1_mm;
constexpr Vec2 kScissorsRingRadii = {2.84_mm, 2.42_mm};

struct ScissorsPiece {
  float side;
  SkPath blade;
  SkPath edge;
  SkPath grip;
  Rect blade_bounds;
  Rect grip_bounds;
  Vec2 tip;
  Vec2 hole;
};

static const ScissorsPiece* ScissorsPieces() {
  auto MakePiece = [](float side) {
    SkMatrix svg_to_box = SkMatrix::Scale(1, side).preConcat(
        SkMatrix::MakeAll(0, 1_mm, -36.489399_mm, 1_mm, 0, -29.308285_mm, 0, 0, 1));
    ScissorsPiece piece = {.side = side};
    piece.blade = SkParsePath::FromSVGString(
                      "m 30.989557,37.81332 c 0,0 -1.371885,-13.799461 -1.410367,-14.230548 "
                      "-0.03847,-0.431086 -0.522389,-0.373071 -0.534137,-0.184892 "
                      "-0.01175,0.188179 -1.692626,17.04336 -1.692626,17.04336 z")
                      ->makeTransform(svg_to_box);
    piece.edge =
        SkParsePath::FromSVGString(
            "m 29.246788,23.256097 c -0.105591,0.0051 -0.196528,0.06134 -0.201538,0.141594 "
            "-0.0073,0.117597 -0.666679,6.744131 -1.160136,11.69851 0.355906,-0.271426 "
            "0.780016,-0.663924 0.803052,-1.032495 0.02706,-0.432994 0.629793,-7.92998 "
            "0.84801,-10.640694 -0.06381,-0.125814 -0.183797,-0.171977 -0.289388,-0.166915 z")
            ->makeTransform(svg_to_box);
    piece.grip =
        SkParsePath::FromSVGString(
            "m 27.568855,38.406077 c -0.104011,3e-5 -0.147253,0.09922 -0.144178,0.269751 "
            "0.006,0.330674 -0.310058,3.468522 -0.706933,4.295345 -0.361648,0.753432 "
            "-0.942981,0.737745 -2.04432,1.504818 -1.172945,0.594583 -1.940967,1.975525 "
            "-1.940967,3.485058 0,2.010874 1.363164,3.793051 3.230294,3.793051 1.51728,0 "
            "2.702101,-1.176882 3.092839,-2.697006 0.0901,-0.350506 0.226501,-0.846214 "
            "0.234769,-1.304478 0.0083,-0.458263 -0.173709,-1.354541 -0.175862,-1.682407 "
            "-0.04871,-2.022551 -0.180868,-6.467379 -0.180868,-6.737057 0,-0.330729 "
            "-0.886716,-0.726284 -1.240234,-0.892968 -0.04884,-0.02303 -0.08987,-0.03412 "
            "-0.12454,-0.03411 z m -1.606104,7.262089 c 0.87272,0 1.730644,0.942874 "
            "1.730644,2.292883 0,1.35001 -0.857924,2.292884 -1.730644,2.292884 -0.872721,0 "
            "-1.730127,-0.942874 -1.730127,-2.292884 0,-1.350009 0.857406,-2.292883 "
            "1.730127,-2.292883 z")
            ->makeTransform(svg_to_box);
    piece.blade_bounds = Rect(piece.blade.getBounds()).Outset(0.3_mm);
    piece.grip_bounds = Rect(piece.grip.getBounds()).Outset(0.3_mm);
    piece.tip = svg_to_box.mapPoint({29.3f, 23.2f});
    piece.hole = svg_to_box.mapPoint({25.962f, 47.961f});
    return piece;
  };
  static const ScissorsPiece pieces[2] = {MakePiece(-1), MakePiece(1)};
  return pieces;
}

void CropScissors::Draw(SkCanvas& canvas) const {
  SkAutoCanvasRestore restore(&canvas, true);
  canvas.concat(BoxToScissors());
  const ScissorsPiece* pieces = ScissorsPieces();
  static RasterPatch blade_patches[2];
  static RasterPatch grip_patches[2];
  auto TipSweep = [](const ScissorsPiece& piece, Span<const SkColor4f> colors,
                     Span<const float> angles) {
    float positions[16];
    for (int i = 0; i < (int)angles.size(); ++i) positions[i] = (angles[i] + 8) / 16;
    SkMatrix tip_to_piece = SkMatrix::Translate(piece.tip.x, piece.tip.y)
                                .preScale(1, piece.side)
                                .preConcat(SkMatrix::RotateDeg(180));
    return SkShaders::SweepGradient({0, 0}, 172, 188,
                                    SkGradient{SkGradient::Colors{{colors.data(), colors.size()},
                                                                  {positions, angles.size()},
                                                                  SkTileMode::kClamp},
                                               {}},
                                    &tip_to_piece);
  };
  auto EdgePaint = [&](const ScissorsPiece& piece) {
    SkColor4f colors[] = {"#3a3c40"_color4f, "#3a3c40"_color4f, "#6a6d73"_color4f};
    float angles[] = {-8, -6.8f, -3.3f};
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setShader(TipSweep(piece, colors, angles));
    return paint;
  };
  static const SkPaint edge_paints[2] = {EdgePaint(pieces[0]), EdgePaint(pieces[1])};
  SinCos box_light = kScissorsLight - (turn + turn_deflection.value);
  SkColor4f lit_color = CropColor(hue, kGripLightTone);
  SkColor4f shaded_color = CropColor(hue, kGripShadeTone);
  SkColor4f dark_color = CropColor(hue, kDarkTone);
  SkPaint plastic_paint;
  plastic_paint.setAntiAlias(true);
  plastic_paint.setColor4f(CropColor(hue, kGripTone));
  auto PatchSize = [](Rect bounds) {
    return SkISize::Make(ceilf(bounds.Width() * kScissorsPatchDensity),
                         ceilf(bounds.Height() * kScissorsPatchDensity));
  };

  auto DrawPicture = [&] {
    for (int i = 0; i < 2; ++i) {
      const ScissorsPiece& piece = pieces[i];
      SinCos piece_turn =
          SinCos::FromDegrees(-piece.side * opening.value * kScissorsOpenAngle.ToDegrees());
      SinCos piece_light = box_light - piece_turn;
      Vec2 light = Vec2::Polar(piece_light, 1);
      canvas.save();
      canvas.concat(piece_turn.ToMatrix());
      {  // Blade
        canvas.save();
        canvas.clipPath(piece.blade, true);
        blade_patches[i].DrawCached(
            canvas, piece.blade_bounds, PatchSize(piece.blade_bounds), [&](SkCanvas& canvas) {
              Rect bounds = piece.blade_bounds;
              {  // Steel
                SkPaint steel;
                steel.setColor4f("#b4b8be"_color4f);
                canvas.drawPaint(steel);
              }
              {  // Reflection along the blade
                SkPoint pts[] = {Vec2(-13_mm, 0), Vec2(4_mm, 0)};
                SkColor4f colors[] = {
                    SkColors::kWhite.withAlpha(0), SkColors::kWhite.withAlpha(0.22f),
                    SkColors::kWhite.withAlpha(0.04f), SkColors::kWhite.withAlpha(0.14f),
                    SkColors::kWhite.withAlpha(0)};
                float positions[] = {0, 0.3f, 0.55f, 0.72f, 1};
                SkPaint reflection;
                reflection.setShader(SkShaders::LinearGradient(
                    pts,
                    SkGradient{SkGradient::Colors{colors, positions, SkTileMode::kClamp}, {}}));
                canvas.drawPaint(reflection);
              }
              {  // Brushed lines
                XorShift32 rng;
                SkPaint line;
                line.setAntiAlias(true);
                line.setStrokeWidth(0.03_mm);
                for (int n = 0; n < 60; ++n) {
                  float y = rng.RollFloat(bounds.bottom, bounds.top);
                  float x = rng.RollFloat(bounds.left, bounds.right);
                  float length = rng.RollFloat(2_mm, 7_mm);
                  bool dark = rng.RollFloat(0, 1) < 0.5f;
                  line.setColor4f((dark ? SkColors::kBlack : SkColors::kWhite)
                                      .withAlpha(rng.RollFloat(0.03f, 0.08f)));
                  canvas.drawLine(Vec2(x, y), Vec2(x + length, y), line);
                }
              }
              {  // Shadow of the grip
                SkPaint shadow;
                shadow.setColor4f(SkColors::kBlack.withAlpha(0.4f));
                shadow.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 0.2_mm));
                canvas.drawPath(piece.grip, shadow);
              }
              return SkPath();
            });
        {  // Light on the flanks of the blade
          float back = std::max(0.f, light.y * piece.side);
          float front = std::max(0.f, -light.y * piece.side);
          SkColor4f white = SkColors::kWhite;
          SkColor4f black = SkColors::kBlack;
          SkColor4f colors[] = {white.withAlpha(0),
                                white.withAlpha(0),
                                white.withAlpha(0.5f * front),
                                white.withAlpha(0.5f * front),
                                white.withAlpha(0),
                                white.withAlpha(0),
                                white.withAlpha(0.55f * back),
                                white.withAlpha(0.55f * back),
                                white.withAlpha(0),
                                black.withAlpha(0),
                                black.withAlpha(0),
                                black.withAlpha(0.3f * front),
                                black.withAlpha(0.3f * front)};
          float angles[] = {-8,   -2.8f, -2.1f, -1.2f, -0.5f, 2.4f, 3.2f,
                            4.7f, 5.4f,  5.4f,  5.7f,  6.6f,  8};
          SkPaint flanks;
          flanks.setShader(TipSweep(piece, colors, angles));
          canvas.drawPaint(flanks);
        }
        canvas.restore();
      }
      canvas.drawPath(piece.edge, edge_paints[i]);
      {  // Grip
        canvas.save();
        canvas.clipPath(piece.grip, true);
        canvas.drawPaint(plastic_paint);
        grip_patches[i].DrawCached(
            canvas, piece.grip_bounds, PatchSize(piece.grip_bounds), [&](SkCanvas& canvas) {
              SkPaint edge;
              edge.setColor4f(SkColors::kBlack.withAlpha(0.4f));
              edge.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 0.45_mm));
              canvas.drawPath(piece.grip.makeToggleInverseFillType(), edge);
              return SkPath();
            });
        {  // Tone
          Rect bounds = piece.grip_bounds;
          Vec2 reach = light * (Length(bounds.Size()) / 2);
          SkPoint pts[] = {bounds.Center() - reach, bounds.Center() + reach};
          SkColor4f colors[] = {shaded_color.withAlpha(0.7f), shaded_color.withAlpha(0),
                                lit_color.withAlpha(0), lit_color.withAlpha(0.45f)};
          float positions[] = {0, 0.5f, 0.5f, 1};
          SkPaint tone;
          tone.setShader(SkShaders::LinearGradient(
              pts, SkGradient{SkGradient::Colors{colors, positions, SkTileMode::kClamp}, {}}));
          canvas.drawPaint(tone);
        }
        SkMatrix ring_to_piece = SkMatrix::Translate(piece.hole.x, piece.hole.y)
                                     .preScale(kScissorsRingRadii.x, kScissorsRingRadii.y);
        SkMatrix light_to_piece =
            SkMatrix::Translate(piece.hole.x, piece.hole.y).preConcat(piece_light.ToMatrix());
        auto RingShader = [&](Span<const SkColor4f> sweep_colors, Span<const float> sweep_positions,
                              Span<const SkColor4f> ring_colors, Span<const float> ring_positions) {
          return SkShaders::Blend(
              SkBlendMode::kDstIn,
              SkShaders::SweepGradient(
                  {0, 0},
                  SkGradient{SkGradient::Colors{{sweep_colors.data(), sweep_colors.size()},
                                                {sweep_positions.data(), sweep_positions.size()},
                                                SkTileMode::kClamp},
                             {}},
                  &light_to_piece),
              SkShaders::RadialGradient(
                  {0, 0}, 1.45f,
                  SkGradient{SkGradient::Colors{{ring_colors.data(), ring_colors.size()},
                                                {ring_positions.data(), ring_positions.size()},
                                                SkTileMode::kDecal},
                             {}},
                  &ring_to_piece));
        };
        SkColor4f lit = lit_color;
        SkColor4f dark = dark_color.withAlpha(0.6f);
        float sweep_positions[] = {0, 0.25f, 0.25f, 0.5f, 0.75f, 0.75f, 1};
        {  // Outer flank of the ring
          SkColor4f sweep[] = {lit,  lit.withAlpha(0),  dark.withAlpha(0),
                               dark, dark.withAlpha(0), lit.withAlpha(0),
                               lit};
          SkColor4f ring[] = {SkColors::kBlack.withAlpha(0), SkColors::kBlack, SkColors::kBlack,
                              SkColors::kBlack.withAlpha(0)};
          float ring_positions[] = {0.66f, 0.82f, 0.88f, 1};
          SkPaint flank;
          flank.setShader(RingShader(sweep, sweep_positions, ring, ring_positions));
          canvas.drawPaint(flank);
        }
        {  // Inner flank of the ring
          SkColor4f sweep[] = {dark, dark.withAlpha(0), lit.withAlpha(0),
                               lit,  lit.withAlpha(0),  dark.withAlpha(0),
                               dark};
          SkColor4f ring[] = {SkColors::kBlack, SkColors::kBlack, SkColors::kBlack.withAlpha(0),
                              SkColors::kBlack.withAlpha(0)};
          float ring_positions[] = {0.45f, 0.55f, 0.66f, 1};
          SkPaint flank;
          flank.setShader(RingShader(sweep, sweep_positions, ring, ring_positions));
          canvas.drawPaint(flank);
        }
        {  // Highlight streak
          SkColor4f white = SkColors::kWhite;
          SkColor4f sweep[] = {white,
                               white.withAlpha(0.7f),
                               white.withAlpha(0),
                               white.withAlpha(0),
                               white.withAlpha(0.7f),
                               white};
          float streak_positions[] = {0, 0.1f, 0.2f, 0.8f, 0.9f, 1};
          SkColor4f ring[] = {white.withAlpha(0), white.withAlpha(0.3f), white.withAlpha(0.3f),
                              white.withAlpha(0)};
          float ring_positions[] = {0.68f, 0.73f, 0.76f, 0.81f};
          SkPaint streak;
          streak.setShader(RingShader(sweep, streak_positions, ring, ring_positions));
          canvas.drawPaint(streak);
        }
        canvas.restore();
      }
      canvas.restore();
    }
    {  // Pivot
      Vec2 light = Vec2::Polar(box_light, 1);
      SkColor4f recess_colors[] = {SkColors::kBlack.withAlpha(0), SkColors::kBlack.withAlpha(0.45f),
                                   SkColors::kBlack.withAlpha(0.12f),
                                   SkColors::kBlack.withAlpha(0)};
      float recess_positions[] = {0.6f, 0.72f, 0.85f, 1};
      SkPaint recess;
      recess.setAntiAlias(true);
      recess.setShader(SkShaders::RadialGradient(
          light * -0.13_mm, 1.2_mm,
          SkGradient{SkGradient::Colors{recess_colors, recess_positions, SkTileMode::kClamp}, {}}));
      canvas.drawCircle(0, 0, 1.2_mm, recess);
      canvas.drawCircle(0, 0, kScissorsPivotRadius, plastic_paint);
      SkColor4f rim_colors[] = {SkColors::kBlack.withAlpha(0), SkColors::kBlack.withAlpha(0.1f),
                                SkColors::kBlack.withAlpha(0.45f)};
      float rim_positions[] = {0, 0.6f, 1};
      SkColor4f light_colors[] = {SkColors::kWhite.withAlpha(0.5f), SkColors::kWhite.withAlpha(0)};
      SkPaint shading;
      shading.setAntiAlias(true);
      shading.setShader(SkShaders::Blend(
          SkBlendMode::kSrcOver,
          SkShaders::RadialGradient(
              light * -0.156_mm, kScissorsPivotRadius,
              SkGradient{SkGradient::Colors{rim_colors, rim_positions, SkTileMode::kClamp}, {}}),
          SkShaders::RadialGradient(
              light * 0.41_mm, 0.55_mm,
              SkGradient{SkGradient::Colors{light_colors, SkTileMode::kClamp}, {}})));
      canvas.drawCircle(0, 0, kScissorsPivotRadius, shading);
    }
  };
  float drawn_distance = halves_distance + halves_distance_deflection.value;
  if (drawn_distance == 0) {
    DrawPicture();
    return;
  }
  {  // Lower half
    canvas.save();
    canvas.translate(0, -drawn_distance / 2);
    canvas.clipRect(Rect(-kScissorsExtent, -kScissorsExtent, kScissorsExtent, 0), true);
    DrawPicture();
    canvas.restore();
  }
  {  // Upper half
    canvas.save();
    canvas.translate(0, drawn_distance / 2);
    canvas.clipRect(Rect(-kScissorsExtent, 0, kScissorsExtent, kScissorsExtent), true);
    DrawPicture();
    canvas.restore();
  }
}

SkPath CropScissors::Shape() const {
  struct PolarOutline {
    float radius[360];
  };
  static const PolarOutline polar_outline = [] {
    PolarOutline outline = {};
    const ScissorsPiece& piece = ScissorsPieces()[1];
    for (const SkPath* path : {&piece.blade, &piece.grip}) {
      SkPathMeasure measure(*path, true);
      do {
        float length = measure.getLength();
        int steps = std::max(8, (int)(length / 0.1_mm));
        SkPoint prev;
        if (!measure.getPosTan(0, &prev, nullptr)) continue;
        for (int s = 1; s <= steps; ++s) {
          SkPoint next;
          if (!measure.getPosTan(length * s / steps, &next, nullptr)) continue;
          for (int degree = 0; degree < 360; ++degree) {
            Vec2 ray = Vec2::Polar(SinCos::FromDegrees(degree), 1);
            Vec2 edge = Vec2(next) - Vec2(prev);
            float denominator = Cross(ray, edge);
            if (denominator == 0) continue;
            float distance = Cross(Vec2(prev), edge) / denominator;
            float along = Cross(Vec2(prev), ray) / denominator;
            if (along >= 0 && along <= 1 && distance > outline.radius[degree]) {
              outline.radius[degree] = distance;
            }
          }
          prev = next;
        }
      } while (measure.nextContour());
    }
    return outline;
  }();
  auto Radius = [&](float degrees) {
    float wrapped = degrees - 360 * floorf(degrees / 360);
    int index = (int)wrapped;
    float fraction = wrapped - index;
    return polar_outline.radius[index % 360] * (1 - fraction) +
           polar_outline.radius[(index + 1) % 360] * fraction;
  };
  float blade_turn = opening.value * kScissorsOpenAngle.ToDegrees();
  float drawn_distance = halves_distance + halves_distance_deflection.value;
  SkPathBuilder builder;
  for (float half : {1.f, -1.f}) {
    for (int degree = 0; degree <= 180; ++degree) {
      float angle = half * degree;
      float radius =
          std::max({Radius(angle + blade_turn), Radius(blade_turn - angle), kScissorsPivotRadius});
      Vec2 point =
          Vec2::Polar(SinCos::FromDegrees(angle), radius) + Vec2(0, half * drawn_distance / 2);
      if (degree == 0) {
        builder.moveTo(point);
      } else {
        builder.lineTo(point);
      }
    }
    builder.close();
  }
  SkMatrix box_to_scissors = BoxToScissors();
  return builder.detach(&box_to_scissors);
}

static uint32_t SumOfTargetMonitors(CropView& view) {
  uint32_t sum = 0;
  for (Interface target = view.scissors.target.Unsafe(); target.object_ptr;) {
    sum += target.object_ptr->monitor.load(std::memory_order_relaxed);
    sum += (uint32_t)(uintptr_t)target.object_ptr;
    if (target.table_ptr != &CropView::croppable_tbl) break;
    target = static_cast<CropView*>(target.object_ptr)->scissors.target.Unsafe();
  }
  return sum;
}

static Linked<Croppable> ChainEndOf(Croppable target) {
  if (target.table_ptr != &CropView::croppable_tbl) return target;
  auto& view = static_cast<CropView&>(*target.object_ptr);
  auto lock = std::lock_guard(view.mutex);
  return view.chain_end;
}

static Toy* FindEndToy(CropScissors& scissors, Interface target, Object& end) {
  Toy* toy = scissors.ToyScope().FindOrNull(*target.object_ptr);
  if (toy && target.table_ptr == &CropView::croppable_tbl) {
    ui::Widget* cropped = toy->FindWidget(target.table_ptr);
    toy = cropped == toy ? nullptr : static_cast<Toy*>(cropped);
  }
  if (!toy || toy->owner.GetUnsafe() != &end) return nullptr;
  return toy;
}

static void SetChainEnd(CropView& view, const Linked<Croppable>& end) {
  {
    auto lock = std::lock_guard(view.mutex);
    if (view.chain_end == end) return;
  }
  std::unique_ptr<CropViewParameters> buffer;
  if (auto croppable = end.Lock()) buffer = croppable.table->crop_view_make_parameters(croppable);
  {
    auto lock = std::lock_guard(view.mutex);
    view.chain_end = end;
    view.parameters = std::move(buffer);
  }
  view.WakeToys();
  Vec<Ptr<CropView>> views;
  {
    auto lock = std::lock_guard(view.croppable.mutex);
    std::erase_if(view.croppable.views,
                  [](const WeakPtr<CropView>& weak) { return weak.IsExpired(); });
    for (auto& weak : view.croppable.views) {
      if (auto next = weak.Lock()) views.push_back(std::move(next));
    }
  }
  for (auto& next : views) SetChainEnd(*next, end);
}

struct CropViewWidget : ObjectToy {
  std::unique_ptr<Toy> cropped;
  std::unique_ptr<CropScissors> scissors;
  float hue;
  uint32_t observed_target_monitors = 0;

  CropViewWidget(ui::Widget* parent, CropView& view)
      : ObjectToy(parent, view),
        scissors(std::make_unique<CropScissors>(*this, view)),
        hue(view.hue) {}

  bool CenteredAtZero() const override { return true; }

  ArcLine Outline() const {
    SkPath inside = cropped ? cropped->Shape().makeTransform(cropped->local_to_parent.asM33())
                            : SkPath::Rect(Rect::MakeCenterZero(kHomeSide, kHomeSide));
    return ArcLine::MakeFromPath(inside).Outset(kViewMargin);
  }

  SkPath Shape() const override { return Outline().ToPath(); }

  Optional<Rect> DrawBounds() const override {
    return Rect(shape.getBounds()).Outset(kViewBoundsOutset);
  }

  bool AllowChildPointerEvents(ui::Widget& child) const override { return &child != cropped.get(); }

  ui::Widget* FindWidget(Interface::Table* table) override {
    if (table == &CropView::croppable_tbl && cropped) return cropped.get();
    return this;
  }

  void OnPoll(time::Timer& timer) override {
    if (cropped) cropped->Poll(timer);
    if (scissors) scissors->Poll(timer);
    auto view = LockObject<CropView>();
    if (!view) return;
    uint32_t sum = SumOfTargetMonitors(*view);
    if (sum != observed_target_monitors) {
      observed_target_monitors = sum;
      WakeAnimationAt(timer.last);
    }
  }

  Tock Tick(time::Timer& timer) override {
    auto view = LockObject<CropView>();
    if (!view) return {};
    Tock tock;
    Linked<Croppable> chain_end;
    {
      auto lock = std::lock_guard(view->mutex);
      chain_end = view->chain_end;
    }
    if (auto end = chain_end.Lock()) {
      bool made = !cropped || cropped->owner.GetUnsafe() != end.object_ptr;
      if (made) {
        cropped = end.table->crop_make_toy(end, *view, this);
        if (auto* location_widget = ui::Closest<LocationWidget>(*this)) {
          if (auto location = location_widget->LockLocation()) {
            location->InvalidateConnectionWidgets(true, false);
          }
        }
      }
      Tock crop_tock = end.table->crop_tick_toy(end, *view, *cropped, timer);
      if (made || crop_tock.shape) cropped->WakeAnimation(&timer);
      tock |= crop_tock;
    } else {
      cropped.reset();
    }
    if (cropped) {
      cropped->shadow_elevation = shadow_elevation + kCroppedElevation;
      Vec2 center = Rect(cropped->Shape().getBounds()).Center();
      cropped->local_to_parent = SkM44::Translate(-center.x, -center.y);
      if (scissors) layers.OrderAbove(scissors.get(), cropped.get());
    }
    hue = view->hue;
    if (Shape() != shape) tock |= Tock::Shape;
    return tock;
  }

  void Draw(SkCanvas&) const override;
};

constexpr float kCropFabricShade = 0.11f;
constexpr float kCropFabricMean = 0.75f;
constexpr float kCropStitchPeriod = 6_mm;
constexpr float kCropStitchLength = 3_mm;
constexpr float kCropStitchWidth = 1_mm;
constexpr float kCropStitchInset = 1_mm;
constexpr float kCropStitchSpriteDensity = 24 / 1_mm;
constexpr Rect kCropStitchSprite = {-2.4_mm, -1.2_mm, 2.4_mm, 1.2_mm};

void CropViewWidget::Draw(SkCanvas& canvas) const {
  ArcLine outline = Outline();
  {  // Fabric
    SkColor4f color = CropColor(hue, kFabricTone);
    float kept = 1 - kCropFabricShade * (1 - kCropFabricMean);
    SkPaint tint;
    tint.setAntiAlias(true);
    tint.setColor4f({color.fR / kept, color.fG / kept, color.fB / kept, 1});
    canvas.drawPath(shape, tint);
    static const SkPaint fabric = [] {
      static auto texture = PersistentImage::MakeFromAsset(
          embedded::assets_crop_fabric_webp,
          {.width = 12.8_mm, .tile_x = SkTileMode::kRepeat, .tile_y = SkTileMode::kRepeat});
      SkPaint paint = texture.paint;
      float matrix[20] = {0, 0, 0, 0,
                          0, 0, 0, 0,
                          0, 0, 0, 0,
                          0, 0, 0, -kCropFabricShade,
                          0, 0, 0, kCropFabricShade};
      paint.setColorFilter(SkColorFilters::Matrix(matrix));
      paint.setAntiAlias(true);
      return paint;
    }();
    canvas.drawPath(shape, fabric);
  }
  {  // Stitches
    static const sk_sp<SkImage> sprites = [] {
      Rect sprite = kCropStitchSprite;
      Rect bounds = {sprite.left, sprite.bottom, sprite.right + sprite.Width(), sprite.top};
      SkPictureRecorder recorder;
      SkCanvas& canvas = *recorder.beginRecording(bounds);
      RRect thread = RRect::MakeSimple(Rect::MakeCenterZero(kCropStitchLength, kCropStitchWidth),
                                       kCropStitchWidth / 2);
      {  // Shadow and pull of the fabric
        SkPaint shadow;
        shadow.setColor4f(SkColors::kBlack.withAlpha(0.5f));
        shadow.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 0.16_mm));
        canvas.drawRRect(thread.sk, shadow);
        SkPaint dimple;
        dimple.setColor4f(SkColors::kBlack.withAlpha(0.22f));
        dimple.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 0.12_mm));
        SkPaint lip;
        lip.setColor4f(SkColors::kWhite.withAlpha(0.14f));
        lip.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 0.08_mm));
        for (float end : {-1.f, 1.f}) {
          Vec2 hole = Vec2(end * kCropStitchLength / 2, 0);
          canvas.drawOval(Rect::MakeCenter(hole, 0.8_mm, 0.6_mm), dimple);
          canvas.drawOval(Rect::MakeCenter(hole + Vec2(end * 0.4_mm, 0), 0.25_mm, 0.7_mm), lip);
        }
      }
      canvas.translate(sprite.Width(), 0);
      {  // Light on the thread
        canvas.clipRRect(thread.sk, true);
        SkPoint pts[] = {Vec2(0, kCropStitchWidth / 2), Vec2(0, -kCropStitchWidth / 2)};
        SkColor4f colors[] = {SkColors::kWhite.withAlpha(0.1f), SkColors::kWhite.withAlpha(0.32f),
                              SkColors::kWhite.withAlpha(0), SkColors::kBlack.withAlpha(0.15f),
                              SkColors::kBlack.withAlpha(0.4f)};
        float positions[] = {0, 0.25f, 0.5f, 0.75f, 1};
        SkPaint profile;
        profile.setShader(SkShaders::LinearGradient(
            pts, SkGradient{SkGradient::Colors{colors, positions, SkTileMode::kClamp}, {}}));
        canvas.drawPaint(profile);
        SkPaint strand;
        strand.setAntiAlias(true);
        strand.setStrokeCap(SkPaint::kRound_Cap);
        SkPaint groove;
        groove.setAntiAlias(true);
        groove.setStrokeWidth(0.08_mm);
        groove.setColor4f(SkColors::kBlack.withAlpha(0.3f));
        groove.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 0.02_mm));
        SkPaint fibre;
        fibre.setAntiAlias(true);
        fibre.setStrokeWidth(0.02_mm);
        Vec2 across = Vec2::Polar(62_deg, kCropStitchWidth * 0.62f);
        Vec2 along = Vec2::Polar(62_deg, 1);
        XorShift32 rng;
        for (float x = -kCropStitchLength / 2 - 0.6_mm; x < kCropStitchLength / 2 + 0.6_mm;
             x += 0.34_mm) {
          strand.setStrokeWidth(0.2_mm);
          strand.setColor4f(SkColors::kWhite.withAlpha(0.08f));
          strand.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 0.05_mm));
          canvas.drawLine(Vec2(x, 0) - across, Vec2(x, 0) + across, strand);
          canvas.drawLine(Vec2(x + 0.17_mm, 0) - across, Vec2(x + 0.17_mm, 0) + across, groove);
          for (int n = 0; n < 3; ++n) {
            Vec2 start = Vec2(x + rng.RollFloat(-0.06_mm, 0.06_mm), 0) +
                         along * rng.RollFloat(-0.5_mm, 0.2_mm);
            fibre.setColor4f(SkColors::kWhite.withAlpha(rng.RollFloat(0.04f, 0.1f)));
            canvas.drawLine(start, start + along * rng.RollFloat(0.15_mm, 0.35_mm), fibre);
          }
        }
        SkPoint end_pts[] = {Vec2(-kCropStitchLength / 2, 0), Vec2(kCropStitchLength / 2, 0)};
        SkColor4f end_colors[] = {
            SkColors::kBlack.withAlpha(0.55f), SkColors::kBlack.withAlpha(0.15f),
            SkColors::kBlack.withAlpha(0),     SkColors::kBlack.withAlpha(0),
            SkColors::kBlack.withAlpha(0.15f), SkColors::kBlack.withAlpha(0.55f)};
        float end_positions[] = {0, 0.1f, 0.22f, 0.78f, 0.9f, 1};
        SkPaint ends;
        ends.setShader(SkShaders::LinearGradient(
            end_pts,
            SkGradient{SkGradient::Colors{end_colors, end_positions, SkTileMode::kClamp}, {}}));
        canvas.drawPaint(ends);
      }
      SkISize size = SkISize::Make(roundf(bounds.Width() * kCropStitchSpriteDensity),
                                   roundf(bounds.Height() * kCropStitchSpriteDensity));
      SkMatrix to_raster = SkMatrix::RectToRect(bounds, SkRect::Make(size));
      return SkImages::DeferredFromPicture(recorder.finishRecordingAsPicture(), size, &to_raster,
                                           nullptr, SkImages::BitDepth::kU8,
                                           SkColorSpace::MakeSRGB());
    }();

    float turning = 0;
    for (int i = 0; i < (int)outline.types.size(); ++i) {
      if (outline.types[i] == ArcLine::Type::Arc) turning += outline.segments[i].arc.ToRadians();
    }
    SinCos inward = turning > 0 ? 90_deg : -90_deg;
    float inset_length =
        ArcLine::Iterator(outline).AdvanceToEnd() - fabsf(turning) * kCropStitchInset;
    int count = std::max(1, (int)roundf(inset_length / kCropStitchPeriod));
    float period = inset_length / count;
    auto ForEachStitch = [&](FnRef<void(Vec2, Vec2)> fn) {
      ArcLine::Iterator it(outline);
      Vec2 prev = it.Position() + Vec2::Polar(it.Angle() + inward, kCropStitchInset);
      float distance = 0;
      int boundary = 0;
      Vec2 start;
      while (boundary < 2 * count) {
        float step = it.Advance(0.2_mm);
        Vec2 next = it.Position() + Vec2::Polar(it.Angle() + inward, kCropStitchInset);
        float segment = Length(next - prev);
        while (boundary < 2 * count && boundary * period / 2 <= distance + segment) {
          float fraction = segment > 0 ? (boundary * period / 2 - distance) / segment : 0;
          Vec2 point = prev + (next - prev) * fraction;
          if (boundary % 2 == 0) {
            start = point;
          } else {
            fn(start, point);
          }
          ++boundary;
        }
        distance += segment;
        prev = next;
        if (step == 0) break;
      }
    };
    Vec2 light = Vec2::Polar(kScissorsLight, 1);
    Vec2 sprite_center =
        Vec2(-kCropStitchSprite.left, -kCropStitchSprite.bottom) * kCropStitchSpriteDensity;
    auto DrawSprites = [&](int sprite, Vec2 offset) {
      constexpr int kBatch = 64;
      SkRSXform transforms[kBatch];
      SkRect texture_rects[kBatch];
      int n = 0;
      SkRect texture_rect =
          SkRect::MakeXYWH(sprite * kCropStitchSprite.Width() * kCropStitchSpriteDensity, 0,
                           kCropStitchSprite.Width() * kCropStitchSpriteDensity,
                           kCropStitchSprite.Height() * kCropStitchSpriteDensity);
      auto Flush = [&] {
        canvas.drawAtlas(
            sprites.get(), {transforms, n}, {texture_rects, n}, {}, SkBlendMode::kSrcOver,
            SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kLinear), nullptr, nullptr);
        n = 0;
      };
      ForEachStitch([&](Vec2 start, Vec2 end) {
        Vec2 along = Normalize(end - start);
        if (Dot(Rotate90DegreesCounterClockwise(along), light) < 0) along = -along;
        float scale = Length(end - start) / kCropStitchLength / kCropStitchSpriteDensity;
        Vec2 center = (start + end) / 2 + offset;
        float scos = along.x * scale;
        float ssin = along.y * scale;
        transforms[n] = SkRSXform::Make(
            scos, ssin, center.x - (scos * sprite_center.x - ssin * sprite_center.y),
            center.y - (ssin * sprite_center.x + scos * sprite_center.y));
        texture_rects[n] = texture_rect;
        if (++n == kBatch) Flush();
      });
      if (n) Flush();
    };
    DrawSprites(0, Vec2(0.1_mm, -0.12_mm));
    {  // Thread
      SkPathBuilder threads;
      ForEachStitch([&](Vec2 start, Vec2 end) {
        Vec2 along = Normalize(end - start) * (kCropStitchWidth / 2);
        threads.moveTo(start + along);
        threads.lineTo(end - along);
      });
      SkPaint thread;
      thread.setAntiAlias(true);
      thread.setStyle(SkPaint::kStroke_Style);
      thread.setStrokeWidth(kCropStitchWidth);
      thread.setStrokeCap(SkPaint::kRound_Cap);
      thread.setColor4f(CropColor(hue, kDarkTone));
      canvas.drawPath(threads.detach(), thread);
    }
    DrawSprites(1, Vec2());
  }
}

CropBox::CropBox(ui::Widget* parent) : Widget(parent) { local_to_parent_weight = 0; }

Optional<Rect> CropBox::DrawBounds() const {
  return Rect::MakeCenterZero(size.x, size.y).Outset(1_mm);
}

void CropBox::Draw(SkCanvas& canvas) const {
  if (size.y == 0) return;
  auto& scissors = static_cast<const CropScissors&>(*parent);
  SkPath outline = SkPath::Rect(Rect::MakeCenterZero(size.x, size.y));
  SkPaint white;
  white.setStyle(SkPaint::kStroke_Style);
  white.setStrokeWidth(0);
  white.setColor4f(SkColors::kWhite);
  canvas.drawPath(outline, white);

  float pixel = 1 / canvas.getTotalMatrix().getMinScale();
  float intervals[] = {8 * pixel, 8 * pixel};
  SkPaint dashes;
  dashes.setStyle(SkPaint::kStroke_Style);
  dashes.setStrokeWidth(0);
  dashes.setColor4f(CropColor(scissors.hue, kDarkTone));
  dashes.setPathEffect(SkDashPathEffect::Make(intervals, 0));
  canvas.drawPath(outline, dashes);
}

CropHandle::CropHandle(ui::Widget* parent) : Widget(parent) { local_to_parent_weight = 0; }

SkPath CropHandle::Corner(Vec2 outward) {
  constexpr float kRadius = 2.5_mm;
  SinCos direction = SinCos::FromVec2(outward);
  Vec2 center = Vec2::Polar(direction, kRadius * 2 * kSqrt1_2);
  return SkPathBuilder()
      .moveTo(0, 0)
      .lineTo(Vec2::Polar(direction - 45_deg, kRadius))
      .arcTo(Rect::MakeCircleR(kRadius).MoveBy(center), (direction - 135_deg).ToDegrees(), 270,
             false)
      .close()
      .detach();
}

SkPath CropHandle::End(Vec2 outward) {
  constexpr float kRadius = 1.5_mm;
  SinCos direction = SinCos::FromVec2(outward);
  Vec2 side = Vec2::Polar(direction - 90_deg, kRadius);
  Vec2 round_center = Vec2::Polar(direction, kRadius);
  return SkPathBuilder()
      .moveTo(side)
      .lineTo(round_center + side)
      .arcTo(Rect::MakeCircleR(kRadius).MoveBy(round_center), (direction - 90_deg).ToDegrees(), 180,
             false)
      .lineTo(-side)
      .close()
      .detach();
}

SkPath CropHandle::Shape() const {
  return outline.makeTransform(SkMatrix::Translate(origin.x, origin.y).preScale(scale, scale));
}

Optional<Rect> CropHandle::DrawBounds() const { return Rect(shape.getBounds()).Outset(1_mm); }

Interface CropHandle::FindOption(ui::Pointer&, ui::ActionTrigger trigger) {
  if (trigger != ui::PointerButton::Left || !option) return {};
  auto view = static_cast<CropScissors&>(*parent).LockOwner<CropView>();
  return view ? Interface(*view, *option) : Interface();
}

void CropHandle::Draw(SkCanvas& canvas) const {
  if (scale == 0) return;
  auto& scissors = static_cast<const CropScissors&>(*parent);
  Rect bounds = shape.getBounds();
  float size = std::max(bounds.Width(), bounds.Height());
  SinCos box_turn = SinCos::FromVec2(local_to_parent.asM33().mapVector(1, 0));
  Vec2 light = Vec2::Polar(kScissorsLight - box_turn, 1);
  SkPoint shade_pts[] = {bounds.Center() + light * (size / 2),
                         bounds.Center() - light * (size / 2)};
  SkColor4f shade_colors[] = {SkColors::kBlack.withAlpha(0), SkColors::kBlack.withAlpha(0.3f)};
  SkColor4f spot = CropColor(scissors.hue, kCropHandleLightTone);
  SkColor4f light_colors[] = {spot.withAlpha(0.6f), spot.withAlpha(0)};
  SkPaint body;
  body.setAntiAlias(true);
  body.setShader(SkShaders::Blend(
      SkBlendMode::kSrcOver,
      SkShaders::Blend(
          SkBlendMode::kSrcOver, SkShaders::Color(CropColor(scissors.hue, kDarkTone).toSkColor()),
          SkShaders::LinearGradient(
              shade_pts, SkGradient{SkGradient::Colors{shade_colors, SkTileMode::kClamp}, {}})),
      SkShaders::RadialGradient(
          bounds.Center() + light * (0.28f * size), 0.6f * size,
          SkGradient{SkGradient::Colors{light_colors, SkTileMode::kClamp}, {}})));
  canvas.drawPath(shape, body);

  SkColor4f rim_colors[] = {"#f9f9f9"_color4f.withAlpha(0.8f), "#f9f9f9"_color4f.withAlpha(0.3f)};
  SkPaint rim;
  rim.setAntiAlias(true);
  rim.setStyle(SkPaint::kStroke_Style);
  rim.setStrokeWidth(0);
  rim.setShader(SkShaders::LinearGradient(
      shade_pts, SkGradient{SkGradient::Colors{rim_colors, SkTileMode::kClamp}, {}}));
  canvas.drawPath(shape, rim);
}

struct DragScissorsAction : Action {
  MortalPtr<CropScissors> scissors;
  ui::Pointer::CursorOverride cursor_override;
  Vec2 offset;
  Vec2 last_touch;

  DragScissorsAction(ui::Pointer& pointer, CropScissors& scissors)
      : Action(pointer), scissors(&scissors), cursor_override(pointer, ui::Cursor::Grabbing) {
    Vec2 position = pointer.PositionWithin(scissors);
    if (scissors.box) offset = scissors.box->center - position;
    last_touch = position + offset;
    scissors.pointer = &pointer;
    scissors.WakeAnimation();
    if (auto* location_widget = ui::Closest<LocationWidget>(*scissors.view_toy)) {
      auto location = location_widget->LockLocation();
      if (auto board = location ? location->LockBoard() : nullptr) board->MoveToTop(*location);
    }
    if (auto* board_widget = BoardOrNull(scissors)) board_widget->WakeAnimation();
  }

  ~DragScissorsAction() override {
    if (!scissors) return;
    scissors->pointer = nullptr;
    scissors->WakeAnimation();
  }

  void Update() override {
    if (!scissors) {
      pointer.ReplaceAction(*this, nullptr);
      return;
    }
    scissors->WakeAnimation();
    auto view = scissors->LockOwner<CropView>();
    if (!view) return;
    Argument scissors_arg = view->scissors.Bind();
    Vec2 position = pointer.PositionWithin(*scissors);
    auto* board_widget = BoardOrNull(*scissors);
    auto LiesOnEnd = [&](Croppable end, Toy& end_toy, Vec2 point) {
      ui::Widget* end_widget = end_toy.FindWidget(end.table_ptr);
      Vec2 on_widget = TransformBetween(*scissors, *end_widget).mapPoint(point);
      return end_widget->shape.contains(on_widget.x, on_widget.y);
    };
    if (auto target = view->scissors->FindInterface()) {
      Linked<Croppable> chain_end;
      {
        auto lock = std::lock_guard(view->mutex);
        chain_end = view->chain_end;
      }
      auto end = chain_end.Lock();
      Toy* end_toy = end ? FindEndToy(*scissors, target, *end.object_ptr) : nullptr;
      Vec2 touch = position + offset;
      if (end_toy && (LiesOnEnd(end, *end_toy, position) || LiesOnEnd(end, *end_toy, touch))) {
        if (touch == last_touch) return;
        last_touch = touch;
        end.table->crop_scissors_move(end, *view, *end_toy, *scissors,
                                      TransformBetween(*scissors, *end_toy).mapPoint(touch));
        return;
      }
      scissors_arg.Disconnect();
      offset = {};
      scissors->base_source = nullptr;
      if (board_widget) board_widget->WakeAnimation();
    }
    auto board = board_widget ? board_widget->LockBoard() : nullptr;
    if (!board) return;
    auto lock = std::lock_guard(engine.mutex);
    if (view->scissors.target) scissors_arg.Disconnect();
    for (auto& location : board->locations) {
      Object& object = *location->object;
      auto table = scissors_arg.CanConnect(object);
      if (!table) continue;
      Croppable candidate = cast<Croppable>(Interface(&object, *table));
      auto end = ChainEndOf(candidate).Lock();
      if (!end) continue;
      Toy* end_toy = FindEndToy(*scissors, candidate, *end.object_ptr);
      if (!end_toy || !LiesOnEnd(end, *end_toy, position)) continue;
      scissors_arg.Connect(candidate);
      end.table->crop_scissors_open(end, *view, *end_toy, *scissors,
                                    TransformBetween(*scissors, *end_toy).mapPoint(position));
      last_touch = position;
      board_widget->WakeAnimation();
      return;
    }
  }

  bool Highlight(Interface end) const override {
    if (!scissors) return false;
    auto view = scissors->LockOwner<CropView>();
    return view && view->scissors->CanConnect(end);
  }
};

CropScissors::CropScissors(ObjectToy& view_toy, Object& view)
    : ArgumentToy(&view_toy, view, CropView::scissors_tbl),
      view_toy(&view_toy),
      base_source(&view_toy),
      turn(kScissorsHomeTurn) {
  local_to_parent_weight = 0;
}

CropScissors::Group* CropScissors::FindGroup(const Linked<Croppable>& croppable) {
  for (auto& group : groups) {
    if (group.croppable == croppable) return &group;
  }
  return nullptr;
}

CropScissors::Group& CropScissors::FindOrMakeGroup(const Linked<Croppable>& croppable) {
  if (Group* group = FindGroup(croppable)) return *group;
  Group& group = groups.emplace_back();
  group.croppable = croppable;
  group.unfolding.value = last_tick == time::SteadyPoint::min() ? 1 : 0;
  return group;
}

ui::Tock CropScissors::Group::Unfold(Vec2 target_center, Vec2 target_size, time::Timer& timer) {
  Tock tock;
  center_deflection.SmoothTargetUpdate(center, target_center);
  size_deflection.SmoothTargetUpdate(size, target_size);
  tock.shaping |= center_deflection.SineTowards({}, timer.d, kBoxPeriod);
  tock.shaping |= size_deflection.SineTowards({}, timer.d, kBoxPeriod);
  if (unfolding.velocity < 0) unfolding.velocity = 0;
  tock.shaping |= unfolding.SineTowards(1, timer.d, kUnfoldingPeriod);
  return tock;
}

ui::Tock CropScissors::Group::Fold(time::Timer& timer) {
  Tock tock;
  tock.shaping |= center_deflection.SineTowards({}, timer.d, kBoxPeriod);
  tock.shaping |= size_deflection.SineTowards({}, timer.d, kBoxPeriod);
  if (unfolding.velocity > 0) unfolding.velocity = 0;
  tock.shaping |= unfolding.SineTowards(0, timer.d, kUnfoldingPeriod);
  if (unfolding.value == 0) gizmos.clear();
  return tock;
}

SkMatrix CropScissors::BoxToScissors() const {
  Vec2 drawn_pivot = pivot + pivot_deflection.value;
  float drawn_scale = scale + scale_deflection.value;
  return SkMatrix::Translate(drawn_pivot.x, drawn_pivot.y)
      .preConcat((turn + turn_deflection.value).ToMatrix())
      .preScale(drawn_scale, drawn_scale);
}

Optional<Rect> CropScissors::DrawBounds() const {
  Rect bounds =
      Rect::MakeCenter({0, 0}, 2 * kScissorsExtent,
                       2 * kScissorsExtent + halves_distance + halves_distance_deflection.value);
  bounds = BoxToScissors().mapRect(bounds.sk);
  for (auto& group : groups) {
    for (auto& gizmo : group.gizmos) {
      if (auto gizmo_bounds = gizmo->DrawBounds()) {
        bounds.ExpandToInclude(gizmo->local_to_parent.asM33().mapRect(*gizmo_bounds));
      }
    }
  }
  return bounds;
}

Interface CropScissors::FindOption(ui::Pointer&, ui::ActionTrigger trigger) {
  if (trigger != ui::PointerButton::Left) return {};
  auto view = LockOwner<CropView>();
  return view ? Interface(view->scissors.Bind()) : Interface();
}

void CropScissors::FillMenu(ui::Pointer& pointer, Menu& menu) {
  if (view_toy) view_toy->FillMenu(pointer, menu);
}

void CropScissors::OnReparent(ui::Widget&, SkM44& fix) {
  SkMatrix matrix = fix.asM33();
  auto MapVector = [&](Vec2 vector) { return Vec2(matrix.mapVector(vector.x, vector.y)); };
  float fix_scale = Length(MapVector(Vec2(1, 0)));
  pivot = matrix.mapPoint(pivot);
  pivot_deflection.value = MapVector(pivot_deflection.value);
  pivot_deflection.velocity = MapVector(pivot_deflection.velocity);
  turn = turn + SinCos::FromVec2(MapVector(Vec2(1, 0)));
  scale *= fix_scale;
  scale_deflection.value *= fix_scale;
  scale_deflection.velocity *= fix_scale;
  for (auto& group : groups) {
    for (auto& gizmo : group.gizmos) gizmo->local_to_parent.postConcat(fix);
  }
  WakeAnimation();
  RedrawThisFrame();
}

void CropScissors::OnPoll(time::Timer& timer) {
  auto view = LockOwner<CropView>();
  if (!view) return;
  uint32_t sum = view->monitor.load(std::memory_order_relaxed) + SumOfTargetMonitors(*view);
  if (sum != observed_target_monitors) {
    observed_target_monitors = sum;
    WakeAnimationAt(timer.last);
  }
}

ui::Tock CropScissors::Tick(time::Timer& timer) {
  if (!view_toy) {
    MarkDead(timer.now);
    return {};
  }
  alpha = parent == view_toy.Get() ? 1 : view_toy->parent->alpha;
  auto view = LockOwner<CropView>();
  if (!view) return {};
  Tock tock = Tock::Shape;
  hue = view->hue;

  ui::Widget* source = view_toy.Get();
  SkMatrix view_to_scissors = TransformBetween(*view_toy, *this);
  Vec2 new_pivot = view_to_scissors.mapPoint(kScissorsHomePivot);
  SinCos new_turn = kScissorsHomeTurn;
  float new_scale = Length(Vec2(view_to_scissors.mapVector(1, 0)));
  float new_elevation = view_toy->shadow_elevation + kElevation;
  Vec2 pointer_position;
  if (pointer) {
    pointer_position = pointer->PositionWithin(*this);
    source = this;
    new_pivot = pointer_position;
    new_turn = 0_deg;
    new_scale = 1;
    new_elevation = kHeldElevation;
  }
  bool first_tick = last_tick == time::SteadyPoint::min();
  box.reset();
  Linked<Croppable> current;
  {  // Current group
    Linked<Croppable> chain_end;
    {
      auto lock = std::lock_guard(view->mutex);
      chain_end = view->chain_end;
    }
    auto end = chain_end.Lock();
    auto target = end ? view->scissors->FindInterface() : Locked<Croppable>();
    if (Toy* end_toy = target ? FindEndToy(*this, target, *end.object_ptr) : nullptr) {
      tock |= end.table->crop_scissors_tick(end, *view, *end_toy, *this, timer);
      if (box) {
        current = chain_end;
        source = end_toy;
        new_pivot = box->center;
        new_turn = box->turn;
        new_scale = box->scale;
        new_elevation = end_toy->shadow_elevation + kElevation;
      }
    }
  }
  for (auto& group : groups) {
    if (group.croppable == current) continue;
    if (auto croppable = group.croppable.Lock()) {
      tock |= croppable.table->crop_scissors_close(croppable, *this, timer);
    } else {
      group.gizmos.clear();
    }
  }
  std::erase_if(groups, [](const Group& group) { return group.gizmos.empty(); });

  if (first_tick) opening.value = box ? 1 : 0;
  {  // Base and remainder of the pose
    float new_halves_distance = box ? box->size.y : 0;
    if (source != base_source && !first_tick) {
      pivot_deflection.value += pivot - new_pivot;
      pivot_deflection.velocity = {};
      turn_deflection.value = turn_deflection.value + (turn - new_turn);
      turn_deflection.velocity = 0_deg;
      scale_deflection.value += scale - new_scale;
      scale_deflection.velocity = 0;
      elevation_deflection.value += elevation - new_elevation;
      halves_distance_deflection.value += halves_distance - new_halves_distance;
      halves_distance_deflection.velocity = 0;
    }
    base_source = source;
    pivot = new_pivot;
    turn = new_turn;
    scale = new_scale;
    elevation = new_elevation;
    halves_distance = new_halves_distance;
    tock.shaping |= pivot_deflection.SineTowards({}, timer.d, kPivotPeriod);
    tock.shaping |= turn_deflection.SineTowards(0_deg, timer.d, kTurnPeriod);
    tock.shaping |= scale_deflection.SineTowards(0, timer.d, kScalePeriod);
    tock.shaping |= halves_distance_deflection.SineTowards(0, timer.d, kHalvesDistancePeriod);
    auto Approach = [&](animation::Kinetic<float>& kinetic, float target, float period) {
      if (kinetic.velocity * (target - kinetic.value) < 0) kinetic.velocity = 0;
      tock.shaping |= kinetic.SineTowards(target, timer.d, period);
    };
    bool apart = halves_distance + halves_distance_deflection.value != 0;
    Approach(opening, box || apart ? 1 : 0, kOpeningPeriod);
    Approach(elevation_deflection, 0, kElevationPeriod);
    shadow_elevation = elevation + elevation_deflection.value;
  }

  for (auto& group : groups) {
    bool current_group = box && group.croppable == current;
    if (current_group) {
      SkM44 box_to_scissors(box->ToScissors());
      for (auto& gizmo : group.gizmos) gizmo->local_to_parent = box_to_scissors;
    }
    for (auto& gizmo : group.gizmos) {
      if (current_group) {
        gizmo->texture_anchors.clear();
      } else if (gizmo->texture_anchors.empty()) {
        gizmo->texture_anchors = {{.pos = {0, 0}, .id = kPivotAnchorId}};
      }
      gizmo->WakeAnimation(&timer);
    }
  }

  {  // Texture anchor at the pivot
    Vec2 drawn_pivot = pivot + pivot_deflection.value;
    LocationWidget* dragged = nullptr;
    if (source != this && source != parent.Get()) {
      dragged = ui::Closest<LocationWidget>(*source);
    }
    const TextureAnchor* grab = dragged ? dragged->GrabAnchor() : nullptr;
    if (pointer) {
      texture_anchors = {{drawn_pivot, kPivotAnchorId, pointer, drawn_pivot - pointer_position,
                          kPivotAnchorDecay, kPivotAnchorRadius}};
    } else if (grab) {
      Vec2 grab_position =
          TransformBetween(*dragged->toy, *this).mapPoint(grab->pos - grab->warp_by);
      texture_anchors = {{drawn_pivot, kPivotAnchorId, grab->pointer, drawn_pivot - grab_position,
                          kPivotAnchorDecay, kPivotAnchorRadius}};
    } else {
      texture_anchors.clear();
    }
  }
  return tock;
}

CropView::~CropView() {
  for (auto& weak : croppable.views) {
    if (auto view = weak.Lock()) SetChainEnd(*view, Linked<Croppable>());
  }
}

Ptr<Object> CropView::Clone() const {
  auto copy = MAKE_PTR(CropView);
  copy->hue = XorShift32::MakeFromCurrentTime().RollFloat(0, 360);
  return copy;
}

std::unique_ptr<ObjectToy> CropView::MakeToy(ui::Widget* parent) {
  return std::make_unique<CropViewWidget>(parent, *this);
}

void CropView::SerializeState(ObjectSerializer& writer) const {
  writer.Key("hue");
  writer.Double(hue);
  Linked<Croppable> end_link;
  {
    auto lock = std::lock_guard(mutex);
    end_link = chain_end;
  }
  auto end = end_link.Lock();
  if (!end) return;
  writer.Key("crop");
  writer.StartObject();
  writer.Key("croppable");
  writer.String(writer.ResolveName(*end.object_ptr, end.table_ptr));
  writer.Key("parameters");
  end.table->crop_view_serialize(end, *this, writer);
  writer.EndObject();
}

bool CropView::DeserializeKey(ObjectDeserializer& d, StrView key) {
  Status status;
  if (key == "hue") {
    d.Get(hue, status);
  } else if (key == "crop") {
    Locked<Croppable> croppable;
    for (auto& field : ObjectView(d, status)) {
      if (field == "croppable") {
        Str name;
        d.Get(name, status);
        if (auto end = d.LookupInterface(name)) croppable = dyn_cast<Croppable>(std::move(end));
        if (croppable) {
          auto buffer = croppable.table->crop_view_make_parameters(croppable);
          auto lock = std::lock_guard(mutex);
          chain_end = croppable;
          parameters = std::move(buffer);
        }
      } else if (field == "parameters" && croppable) {
        croppable.table->crop_view_deserialize(croppable, *this, d);
      } else {
        d.Skip();
      }
    }
    WakeToys();
  } else {
    return false;
  }
  if (!OK(status)) ReportError(status.ToStr());
  return true;
}

void CropView::scissors_Impl::OnCanConnect(Interface end, Status& status) {
  if (!dyn_cast_if_present<Croppable::Table>(end.table_ptr)) {
    AppendErrorMessage(status) += "Wrong interface type";
    return;
  }
  Locked<Croppable> next;
  for (Interface target = end; target.table_ptr == &CropView::croppable_tbl; target = next) {
    auto& view = static_cast<CropView&>(*target.object_ptr);
    if (&view == &*obj) {
      AppendErrorMessage(status) += "A Crop View cannot crop itself";
      return;
    }
    next = view.scissors->FindInterface();
  }
}

void CropView::scissors_Impl::OnConnect(Interface end) {
  Croppable target = dyn_cast_if_present<Croppable>(end);
  Linked<Croppable> target_link = target;
  if (target_link == state->target) return;
  if (auto old_target = state->target.Lock()) {
    auto lock = std::lock_guard(old_target.state->mutex);
    std::erase_if(old_target.state->views,
                  [&](const WeakPtr<CropView>& view) { return view.GetUnsafe() == &*obj; });
  }
  if (target) {
    auto lock = std::lock_guard(target.state->mutex);
    std::erase_if(target.state->views,
                  [](const WeakPtr<CropView>& view) { return view.IsExpired(); });
    target.state->views.push_back(obj->AcquireWeakPtr());
  }
  bool had_target = (bool)state->target;
  state->target = std::move(target_link);
  if (had_target) SetChainEnd(*obj, Linked<Croppable>());
  if (target) {
    if (auto new_end = ChainEndOf(target)) SetChainEnd(*obj, new_end);
  }
}

std::unique_ptr<ArgumentToy> CropView::scissors_Impl::OnMakeToy(ui::Widget* parent) {
  auto& view_toy = static_cast<CropViewWidget&>(*parent->ToyScope().FindOrNull(*obj));
  view_toy.scissors->Reparent(*parent);
  return std::move(view_toy.scissors);
}

void CropView::scissors_Impl::OnDestroyToy(std::unique_ptr<automat::Toy> toy) {
  auto& scissors = static_cast<CropScissors&>(*toy);
  if (!scissors.view_toy) return;
  auto& view_toy = static_cast<CropViewWidget&>(*scissors.view_toy);
  view_toy.scissors.reset(static_cast<CropScissors*>(toy.release()));
  scissors.Reparent(view_toy);
}

std::unique_ptr<Action> CropView::scissors_Impl::OnActivate(ui::Pointer& pointer,
                                                            automat::Toy* toy) {
  auto* scissors = dynamic_cast<CropScissors*>(toy);
  return scissors ? std::make_unique<DragScissorsAction>(pointer, *scissors) : nullptr;
}

}  // namespace automat
