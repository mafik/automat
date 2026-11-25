// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "library_beta_shelf.hpp"

#include <include/core/SkBitmap.h>
#include <include/core/SkBlendMode.h>
#include <include/core/SkBlurTypes.h>
#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/core/SkImage.h>
#include <include/core/SkMaskFilter.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPath.h>
#include <include/core/SkPathBuilder.h>
#include <include/core/SkRSXform.h>
#include <include/core/SkShader.h>
#include <include/effects/SkGradient.h>
#include <include/effects/SkImageFilters.h>
#include <include/effects/SkRuntimeEffect.h>

#include "animation.hpp"
#include "arcline.hpp"
#include "color.hpp"
#include "drawing.hpp"
#include "embedded.hpp"
#include "global_resources.hpp"
#include "log.hpp"
#include "math.hpp"
#include "menu.hpp"
#include "object_source.hpp"
#include "pointer.hpp"
#include "prototypes.hpp"
#include "random.hpp"
#include "sincos.hpp"
#include "status.hpp"
#include "units.hpp"

namespace automat::library {

constexpr float kBottom = -17_mm;
constexpr float kBottomHalfWidth = 12_mm;
constexpr float kBottomCornerRadius = 2.5_mm;
constexpr float kNeckBottom = 4_mm;
constexpr float kShoulderRadius = 4_mm;
constexpr float kNeckHalfWidth = 4.5_mm;
constexpr float kNeckTop = 12_mm;
constexpr float kLipHalfWidth = kNeckHalfWidth + 0.6_mm;
constexpr float kLipTop = kNeckTop + 1_mm;
constexpr float kLipRadius = (kLipTop - kNeckTop) / 2;
constexpr float kCorkBottom = 10_mm;
constexpr float kCorkTop = 17_mm;
constexpr float kCorkLipHalfWidth = 4.2_mm;
constexpr float kCorkTopHalfWidth = 4.6_mm;
constexpr float kCorkCornerRadius = 0.6_mm;
constexpr float kCorkShadowSide = 1_mm;
constexpr float kCorkBottomHalfWidth =
    kCorkLipHalfWidth -
    (kLipTop - kCorkBottom) * (kCorkTopHalfWidth - kCorkLipHalfWidth) / (kCorkTop - kLipTop);
constexpr float kGlassThickness = 0.3_mm;

constexpr Vec2 kCone = {kNeckHalfWidth - kBottomHalfWidth, kNeckBottom - kBottom};
constexpr Rect kGlassBounds = {-kBottomHalfWidth, kBottom, kBottomHalfWidth, kLipTop};
constexpr Rect kCorkBounds = {-kCorkTopHalfWidth, kCorkBottom, kCorkTopHalfWidth, kCorkTop};
constexpr Rect kInsideBounds = {-kBottomHalfWidth + kGlassThickness, kBottom + kGlassThickness,
                                kBottomHalfWidth - kGlassThickness, kCorkBottom};

constexpr int kParticles = 256;
constexpr int kMaxPairs = kParticles * 16;
constexpr float kStepTime = 1 / 360.f;
constexpr Vec2 kGravity = {0, -1.2_m};
constexpr float kFollowTime = 0.04f;
constexpr float kFlaskAccelerationScale = 0.5f;
constexpr float kMaxLiquidAcceleration = 2.5_m;
constexpr float kLatticeSpacing = 0.78_mm;
constexpr float kInteractionRadius = 2_mm;
constexpr float kSurfaceDistance = 0.75_mm;
constexpr float kWallDistance = 0.4_mm;
static const SinCos kWallPointAngle = 15_deg;
constexpr float kParallelWallSine = 0.1f;
constexpr float kReferenceDensity = 2.12f;
constexpr float kStiffness = 40_m;
constexpr float kNearStiffness = 6_m;
constexpr float kMinPressure = -4_m;
constexpr int kRelaxationIterations = 2;
constexpr float kLinearViscosity = 20;
constexpr float kDragTime = 0.5f;
constexpr float kMaxSpeed = 0.2_m;
constexpr float kMaxDisplacement = 0.2_mm;
constexpr float kSettledSpeed = 4_mm;
constexpr float kSheenShear = 20_mm;
constexpr float kSheenRiseTime = 0.05f;
constexpr float kSheenFadeTime = 0.6f;
constexpr float kSettleTime = 2.1f;

constexpr float kMaxTickTime = 1 / 30.f;
constexpr float kJumpSpeed = 2_m;
constexpr float kLevelRiseTime = 0.5f;
constexpr float kLevelFallTime = 0.1f;
constexpr Vec2 kLiquidCenter = {0, -12_mm};
constexpr float kSpriteRadius = 1.6_mm;
constexpr float kSpritePeak = 0.55f;
constexpr int kSpriteSize = 64;
constexpr float kDrawBoundsOutset = 0.5_mm;
constexpr float kGlassPatchResolution = 12 / 1_mm;
constexpr float kCorkPatchResolution = 24 / 1_mm;

constexpr SkColor4f kGlassTint = "#203c35"_color4f;
constexpr SkColor4f kGlassEdge = "#0e1f1b"_color4f;
constexpr SkColor4f kCoolLight = "#d7ecff"_color4f;
constexpr SkColor4f kLiquidLight = "#8ccba5"_color4f;
constexpr SkColor4f kCorkLight = "#dcb987"_color4f;
constexpr SkColor4f kCork = "#b88a58"_color4f;
constexpr SkColor4f kCorkShade = "#5a391e"_color4f;
constexpr SkColor4f kCorkReflection = "#a07a52"_color4f;
constexpr SkColor4f kCorkPore = "#4e3018"_color4f;

static Interface Proto(StrView name) {
  auto* proto = prototypes ? prototypes->Find(name) : nullptr;
  return proto ? Interface(*proto, kMakeObject) : Interface();
}

static void FillFfmpegMenu(Interface, Interface::Table&, Menu& menu) {
  using enum ui::Dir;
  menu.Place(N, Proto("avformat"));
  menu.Place(S, Proto("avcodec"));
}

static void FillTensorFlowMenu(Interface, Interface::Table&, Menu& menu) {
  using enum ui::Dir;
  menu.Place(N, Proto("tf:tensor"));
  menu.Place(S, Proto("Square"));
}

static void FillPipelinesMenu(Interface self, Interface::Table&, Menu& menu) {
  using enum ui::Dir;
  menu.Place(NW, Proto("GStreamer"));
  menu.Place(N, Interface(self.object_ptr, &kFfmpegMenu));
  menu.Place(NE, Interface(self.object_ptr, &kTensorFlowMenu));
  menu.Place(SW, Proto("GEGL"));
  menu.Place(SE, Proto("PipeWire"));
  menu.Place(S, Proto("pipewire:node"));
}

static float TanHalf(SinCos angle) { return (1 - (float)angle.cos) / (float)angle.sin; }

static float ConeX(float y, float inset) {
  return kBottomHalfWidth - inset * Length(kCone) / kCone.y + (y - kBottom) * kCone.x / kCone.y;
}

static void AddGlassBody(ArcLine& outline, float neck_top, float inset) {
  SinCos cone = SinCos::FromVec2(kCone);
  SinCos shoulder = 90_deg - cone;
  float corner_radius = kBottomCornerRadius - inset;
  float shoulder_radius = kShoulderRadius + inset;
  float neck_x = kNeckHalfWidth - inset;
  Vec2 corner = {ConeX(kBottom + inset, inset), kBottom + inset};
  Vec2 shoulder_point = {neck_x, kBottom + (neck_x - ConeX(kBottom, inset)) * kCone.y / kCone.x};
  float corner_length = corner_radius * TanHalf(cone);
  float shoulder_length = shoulder_radius * TanHalf(-shoulder);
  float cone_length = Length(shoulder_point - corner) - corner_length - shoulder_length;
  float neck_length = neck_top - shoulder_point.y - shoulder_length;
  outline.MoveBy(neck_length);
  outline.TurnConvex(shoulder, shoulder_radius);
  outline.MoveBy(cone_length);
  outline.TurnConvex(cone, corner_radius);
  outline.MoveBy(2 * (corner.x - corner_length));
  outline.TurnConvex(cone, corner_radius);
  outline.MoveBy(cone_length);
  outline.TurnConvex(shoulder, shoulder_radius);
  outline.MoveBy(neck_length);
}

static ArcLine GlassOutline() {
  ArcLine outline({-kCorkLipHalfWidth, kLipTop}, 180_deg);
  outline.MoveBy(kLipHalfWidth - kLipRadius - kCorkLipHalfWidth);
  outline.TurnConvex(180_deg, kLipRadius);
  outline.MoveBy(kLipHalfWidth - kLipRadius - kNeckHalfWidth);
  outline.TurnConvex(-90_deg, 0);
  AddGlassBody(outline, kNeckTop, 0);
  outline.TurnConvex(-90_deg, 0);
  outline.MoveBy(kLipHalfWidth - kLipRadius - kNeckHalfWidth);
  outline.TurnConvex(180_deg, kLipRadius);
  outline.MoveBy(kLipHalfWidth - kLipRadius - kCorkLipHalfWidth);
  return outline;
}

static ArcLine InsideOutline() {
  ArcLine outline({-(kNeckHalfWidth - kGlassThickness), kCorkBottom}, -90_deg);
  AddGlassBody(outline, kCorkBottom, kGlassThickness);
  outline.TurnConvex(90_deg, 0);
  outline.MoveBy(2 * (kNeckHalfWidth - kGlassThickness));
  outline.TurnConvex(90_deg, 0);
  return outline;
}

static float DistanceToWall(Vec2 point, Vec2& inward) {
  static const Vec<Vec2> wall = [] {
    const float step_radians = kWallPointAngle.ToRadians();
    ArcLine inside = InsideOutline();
    ArcLine::Iterator it(inside);
    Vec<Vec2> points;
    for (int i = 0; i < inside.types.size(); ++i) {
      if (inside.types[i] == ArcLine::Type::Line) {
        points.push_back(it.Position());
        it.Advance(inside.segments[i].line.length);
        continue;
      }
      auto& arc = inside.segments[i].arc;
      float radians = fabsf(arc.ToRadians());
      float length = fabsf(arc.radius) * radians;
      if (length == 0) continue;
      int steps = ceilf(radians / step_radians);
      for (int step = 0; step < steps; ++step) {
        points.push_back(it.Position());
        it.Advance(length / steps);
      }
    }
    return points;
  }();
  float closest_squared = INFINITY;
  bool inside = false;
  Vec2 a = wall.back();
  for (Vec2 b : wall) {
    Vec2 edge = b - a;
    Vec2 offset = point - a;
    Vec2 from_wall = offset - edge * Saturate(SegmentProjection(a, b, point));
    float distance_squared = LengthSquared(from_wall);
    if (distance_squared < closest_squared) {
      closest_squared = distance_squared;
      inward = distance_squared > 0 ? from_wall : Rotate90DegreesCounterClockwise(edge);
    }
    if ((a.y > point.y) != (b.y > point.y) && (Cross(edge, offset) > 0) == (edge.y > 0)) {
      inside = !inside;
    }
    a = b;
  }
  inward = Normalize(inward);
  if (!inside) inward = -inward;
  float distance = sqrtf(closest_squared);
  return inside ? distance : -distance;
}

struct BetaShelfToy : ObjectToy {
  struct Liquid {
    Vec2 position[kParticles];
    Vec2 velocity[kParticles];
    float sheen[kParticles];
    Vec2 flask_velocity;
    float unsimulated_time;

    animation::Progress Simulate(float delta_time, Vec2 target_flask_velocity);
    float Level() const;
  };

  static const Liquid& SettledLiquid();

  Liquid liquid = SettledLiquid();
  Vec2 window_position;
  float level_weight = 1;
  float level = liquid.Level();

  static inline RasterPatch glass_patch;
  static inline RasterPatch cork_patch;

  BetaShelfToy(ui::Widget* parent, Object& obj) : ObjectToy(parent, obj) {}

  StrView Name() const override { return "BetaShelfToy"; }

  SkPath Shape() const override {
    static const SkPath shape = [] {
      Vec2 cork_side = {kCorkTopHalfWidth - kCorkLipHalfWidth, kCorkTop - kLipTop};
      SinCos cork_corner = 180_deg - SinCos::FromVec2(cork_side);
      float corner_length = kCorkCornerRadius * TanHalf(cork_corner);
      float side_length = Length(cork_side) - corner_length;
      ArcLine outline = GlassOutline();
      outline.TurnConvex(-cork_corner, 0);
      outline.MoveBy(side_length);
      outline.TurnConvex(cork_corner, kCorkCornerRadius);
      outline.MoveBy(2 * (kCorkTopHalfWidth - corner_length));
      outline.TurnConvex(cork_corner, kCorkCornerRadius);
      outline.MoveBy(side_length);
      outline.TurnConvex(-cork_corner, 0);
      return outline.ToPath();
    }();
    return shape;
  }

  Optional<Rect> DrawBounds() const override {
    return Rect(Shape().getBounds()).Outset(kDrawBoundsOutset);
  }

  void TransformUpdated(time::Timer& timer) override { WakeAnimationAt(timer.last); }

  Tock Tick(time::Timer& timer) override {
    Tock tock;
    Vec2 position = local_to_window.mapPoint(kLiquidCenter);
    for (auto& anchor : texture_anchors) {
      auto* pointer = anchor.pointer.Get();
      if (!pointer) continue;
      position += pointer->pointer_position - Vec2(local_to_window.mapPoint(anchor.pos)) +
                  Vec2(local_to_window.mapVector(anchor.warp_by));
      tock.ing |= true;
    }
    if (last_tick == time::SteadyPoint::min()) {
      window_position = position;
      return tock;
    }
    float d = std::min<float>(timer.d, kMaxTickTime);
    SkMatrix window_to_local;
    (void)local_to_window.invert(&window_to_local);
    Vec2 displacement = window_to_local.mapVector(position - window_position);
    Vec2 flask_velocity = timer.d <= 0 || Length(displacement) > kJumpSpeed * d
                              ? liquid.flask_velocity
                              : displacement / (float)timer.d;
    tock.ing |= position != window_position;
    window_position = position;
    bool calm = Length(flask_velocity - liquid.flask_velocity) < kSettledSpeed;
    if (level_weight == 1 && calm) return tock;
    auto progress = liquid.Simulate(d, flask_velocity);
    if (!progress.settled) {
      tock.drawing |= animation::LinearApproach(0, d, 1 / kLevelFallTime, level_weight);
    } else if (calm) {
      level = liquid.Level();
      tock.drawing |= animation::LinearApproach(1, d, 1 / kLevelRiseTime, level_weight);
    }
    if (level_weight < 1) {
      tock |= Tock::Drawing;
    } else {
      std::fill_n(liquid.sheen, kParticles, 0);
    }
    return tock;
  }

  void Draw(SkCanvas& canvas) const override {
    static const SkPath glass = GlassOutline().ToPath(false);
    static const SkPath inside = InsideOutline().ToPath();

    {  // Liquid
      Status status;
      static auto effect = resources::CompileShader(embedded::assets_flask_liquid_sksl, status);
      if (!OK(status)) FATAL << status;
      SkMatrix local_to_device = canvas.getLocalToDeviceAs3x3();
      SkMatrix device_to_local;
      (void)local_to_device.invert(&device_to_local);
      Rect bounds_px = local_to_device.mapRect(inside.getBounds());
      SkRuntimeEffectBuilder builder(effect);
      builder.uniform("iDeviceToLocal") = device_to_local;
      builder.uniform("iLevel") = level;
      builder.uniform("iLevelWeight") = level_weight;
      builder.uniform("iSpriteRadius") = kSpriteRadius;
      builder.uniform("iConeHalfWidth") = ConeX(0, kGlassThickness);
      builder.uniform("iConeSlope") = kCone.x / kCone.y;
      builder.uniform("iInsideBottom") = kInsideBounds.bottom;
      builder.uniform("iInsideNeckHalfWidth") = kNeckHalfWidth - kGlassThickness;
      builder.uniform("iCorkBottom") = kCorkBottom;
      SkPaint paint;
      paint.setImageFilter(SkImageFilters::RuntimeShader(builder, "iParticles", nullptr));
      canvas.save();
      canvas.clipPath(inside, true);
      canvas.resetMatrix();
      canvas.saveLayer(SkCanvas::SaveLayerRec(&bounds_px.sk, &paint, SkCanvas::kF16ColorType));
      if (level_weight < 1) {
        static const sk_sp<SkImage> sprite = [] {
          SkBitmap bitmap;
          bitmap.allocN32Pixels(kSpriteSize, kSpriteSize);
          for (int y = 0; y < kSpriteSize; ++y) {
            for (int x = 0; x < kSpriteSize; ++x) {
              Vec2 offset = Vec2(x + 0.5f, y + 0.5f) / (kSpriteSize / 2.f) - Vec2(1, 1);
              float falloff = std::max(1 - LengthSquared(offset), 0.f);
              float alpha = kSpritePeak * falloff * falloff * falloff;
              *bitmap.getAddr32(x, y) = SkPreMultiplyARGB(roundf(alpha * 255), 255, 255, 255);
            }
          }
          bitmap.setImmutable();
          return SkImages::RasterFromBitmap(bitmap);
        }();
        float scale = local_to_device.mapRadius(kSpriteRadius) / (kSpriteSize / 2.f);
        SkRSXform transforms[kParticles];
        SkRect sprite_rects[kParticles];
        SkColor colors[kParticles];
        for (int i = 0; i < kParticles; ++i) {
          Vec2 center = local_to_device.mapPoint(liquid.position[i]);
          transforms[i] = SkRSXform::Make(scale, 0, center.x - scale * kSpriteSize / 2,
                                          center.y - scale * kSpriteSize / 2);
          sprite_rects[i] = SkRect::MakeWH(kSpriteSize, kSpriteSize);
          colors[i] = SkColorSetARGB(255, roundf(liquid.sheen[i] * 255), 0, 0);
        }
        canvas.drawAtlas(sprite.get(), transforms, sprite_rects, colors, SkBlendMode::kModulate,
                         SkSamplingOptions(SkFilterMode::kLinear), &bounds_px.sk, nullptr);
      }
      canvas.restore();
      canvas.restore();
    }

    {  // Cork
      static const SkPath cork = [] {
        SkPathBuilder builder;
        builder.moveTo(-kCorkBottomHalfWidth, kCorkBottom);
        builder.lineTo(kCorkBottomHalfWidth, kCorkBottom);
        builder.arcTo({kCorkTopHalfWidth, kCorkTop}, {0, kCorkTop}, kCorkCornerRadius);
        builder.arcTo({-kCorkTopHalfWidth, kCorkTop}, {-kCorkBottomHalfWidth, kCorkBottom},
                      kCorkCornerRadius);
        builder.close();
        return builder.detach();
      }();
      canvas.save();
      canvas.clipPath(cork, true);
      SkISize patch_size(roundf(kCorkBounds.Width() * kCorkPatchResolution),
                         roundf(kCorkBounds.Height() * kCorkPatchResolution));
      cork_patch.DrawCached(canvas, kCorkBounds, patch_size, [](SkCanvas& canvas) {
        SkPoint shading_pts[] = {kCorkBounds.LeftCenter(), kCorkBounds.RightCenter()};
        SkColor4f shading_colors[] = {kCork, kCorkLight, kCorkLight,
                                      kCork, kCorkShade, kCorkReflection};
        float shading_positions[] = {0, 0.25f, 0.38f, 0.62f, 0.88f, 1};
        SkPaint cork_shading;
        cork_shading.setShader(SkShaders::LinearGradient(
            shading_pts,
            SkGradient{SkGradient::Colors{shading_colors, shading_positions, SkTileMode::kClamp},
                       {}}));
        canvas.drawPaint(cork_shading);

        XorShift32 rng;
        rng.state = 0x9e3779b9;
        auto DrawPore = [&](Vec2 center, Vec2 size, SkColor4f color) {
          Rect bounds = Rect::MakeCenter(center, size.x, size.y);
          SkMatrix pore_matrix = SkMatrix::RectToRect(Rect::MakeCircleR(1), bounds);
          SkColor4f colors[] = {color, color.withAlpha(color.fA * 0.6f), color.withAlpha(0)};
          float positions[] = {0, 0.5f, 1};
          SkPaint paint;
          paint.setShader(SkShaders::RadialGradient(
              {0, 0}, 1, SkGradient{SkGradient::Colors{colors, positions, SkTileMode::kClamp}, {}},
              &pore_matrix));
          canvas.drawOval(bounds, paint);
        };
        for (int i = 0; i < 1000; ++i) {
          Vec2 center(rng.RollFloat(kCorkBounds.left, kCorkBounds.right),
                      rng.RollFloat(kCorkBounds.bottom, kCorkBounds.top));
          float size = rng.RollFloat(0.08_mm, 0.19_mm);
          bool dark = rng.Roll() % 2;
          float alpha = dark ? rng.RollFloat(0.1f, 0.25f) : rng.RollFloat(0.15f, 0.35f);
          if (!dark && center.x > kCorkShadowSide) continue;
          DrawPore(center, {size, size}, (dark ? kCorkPore : kCorkLight).withAlpha(alpha));
        }
        for (int i = 0; i < 220; ++i) {
          Vec2 center(rng.RollFloat(kCorkBounds.left, kCorkBounds.right),
                      rng.RollFloat(kCorkBounds.bottom, kCorkBounds.top));
          float size = rng.RollFloat(0.08_mm, 0.22_mm);
          DrawPore(center, {size * rng.RollFloat(1.5f, 3.5f), size},
                   kCorkPore.withAlpha(rng.RollFloat(0.2f, 0.5f)));
        }

        SkPoint contact_pts[] = {Vec2(0, kLipTop), Vec2(0, kLipTop + 0.5_mm)};
        SkColor4f contact_colors[] = {kCorkPore.withAlpha(0.22f), kCorkPore.withAlpha(0)};
        SkPaint contact_shadow;
        contact_shadow.setShader(SkShaders::LinearGradient(
            contact_pts, SkGradient{SkGradient::Colors{contact_colors, SkTileMode::kClamp}, {}}));
        canvas.drawRect(Rect{kCorkBounds.left, kLipTop, kCorkBounds.right, kLipTop + 0.5_mm},
                        contact_shadow);

        SkPoint top_pts[] = {Vec2(0, kCorkTop), Vec2(0, kCorkTop - 0.8_mm)};
        SkColor4f top_colors[] = {kCorkLight.withAlpha(0.6f), kCorkLight.withAlpha(0)};
        SkPaint top_light;
        top_light.setShader(SkShaders::LinearGradient(
            top_pts, SkGradient{SkGradient::Colors{top_colors, SkTileMode::kClamp}, {}}));
        canvas.drawRect(Rect{kCorkBounds.left, kCorkTop - 0.8_mm, kCorkBounds.right, kCorkTop},
                        top_light);

        return SkPath();
      });
      canvas.restore();
    }

    {  // Glass
      float pixel = 1 / canvas.getLocalToDeviceAs3x3().mapRadius(1);
      auto DrawStroke = [&](const SkPath& path, const SkPaint& paint) {
        float width = paint.getStrokeWidth();
        if (width >= pixel) {
          canvas.drawPath(path, paint);
          return;
        }
        SkPaint widened = paint;
        widened.setStrokeWidth(pixel);
        widened.setAlphaf(paint.getAlphaf() * width / pixel);
        canvas.drawPath(path, widened);
      };
      canvas.save();
      canvas.clipPath(glass, true);
      SkISize patch_size(roundf(kGlassBounds.Width() * kGlassPatchResolution),
                         roundf(kGlassBounds.Height() * kGlassPatchResolution));
      glass_patch.DrawCached(canvas, kGlassBounds, patch_size, [](SkCanvas& canvas) {
        canvas.clipPath(glass, true);
        SkPaint haze;
        haze.setColor4f(SkColors::kWhite.withAlpha(0.04f));
        canvas.drawPaint(haze);

        canvas.save();
        canvas.clipRect(Rect{kGlassBounds.left, kBottom, kGlassBounds.right, kNeckTop});
        SkPaint edge_tint;
        edge_tint.setColor4f(kGlassTint.withAlpha(0.2f));
        edge_tint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 0.35_mm));
        canvas.drawPath(glass.makeToggleInverseFillType(), edge_tint);
        canvas.save();
        canvas.translate(-0.5_mm, 0.5_mm);
        edge_tint.setColor4f(kGlassTint.withAlpha(0.17f));
        edge_tint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 0.7_mm));
        canvas.drawPath(glass.makeToggleInverseFillType(), edge_tint);
        canvas.restore();
        canvas.restore();

        Vec2 left_cone = {-kCone.x, kCone.y};
        Vec2 inward = Normalize(Rotate90DegreesClockwise(left_cone));
        auto LeftWallPoint = [&](float along, float offset) {
          return Vec2(-kBottomHalfWidth, kBottom) + left_cone * along + inward * offset;
        };
        struct {
          Vec2 start;
          Vec2 end;
          float width;
          float blur;
          SkColor4f color;
          float end_alpha;
          bool mirrored;
        } bands[] = {
            {LeftWallPoint(0.06f, kGlassThickness + 0.05_mm),
             LeftWallPoint(0.9f, kGlassThickness + 0.05_mm), 1.9_mm, 0.06_mm, SkColors::kWhite, 0,
             false},
            {LeftWallPoint(0.08f, kGlassThickness + 1.6_mm),
             LeftWallPoint(0.85f, kGlassThickness + 1.6_mm), 3.5_mm, 0.8_mm,
             SkColors::kWhite.withAlpha(0.14f), 0, false},
            {LeftWallPoint(0.1f, kGlassThickness), LeftWallPoint(0.9f, kGlassThickness), 1.2_mm,
             0.15_mm, kCoolLight.withAlpha(0.1f), 0, true},
            {{-kNeckHalfWidth + kGlassThickness + 0.4_mm, kNeckBottom + 1.5_mm},
             {-kNeckHalfWidth + kGlassThickness + 0.4_mm, kNeckTop},
             0.5_mm,
             0.1_mm,
             SkColors::kWhite.withAlpha(0.6f),
             0.4f,
             false},
            {{-kNeckHalfWidth + kGlassThickness, kNeckBottom + 1_mm},
             {-kNeckHalfWidth + kGlassThickness, kNeckTop},
             1.2_mm,
             0.15_mm,
             kCoolLight.withAlpha(0.1f),
             0,
             true},
        };
        for (auto& band : bands) {
          Vec2 delta = band.end - band.start;
          float length = Length(delta);
          SkPoint across_pts[] = {{0, 0}, {0, -band.width}};
          SkColor4f across_colors[] = {band.color, band.color.withAlpha(band.color.fA * 0.4f),
                                       band.color.withAlpha(0)};
          float across_positions[] = {0, 0.2f, 1};
          SkPoint along_pts[] = {{0, 0}, {length, 0}};
          SkColor4f along_colors[] = {SkColors::kWhite.withAlpha(0), SkColors::kWhite,
                                      SkColors::kWhite, SkColors::kWhite.withAlpha(band.end_alpha)};
          float along_positions[] = {0, 0.25f, 0.7f, 1};
          SkPaint paint;
          paint.setShader(SkShaders::Blend(
              SkBlendMode::kDstIn,
              SkShaders::LinearGradient(
                  across_pts,
                  SkGradient{
                      SkGradient::Colors{across_colors, across_positions, SkTileMode::kClamp}, {}}),
              SkShaders::LinearGradient(
                  along_pts,
                  SkGradient{SkGradient::Colors{along_colors, along_positions, SkTileMode::kClamp},
                             {}})));
          paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, band.blur));
          canvas.save();
          if (band.mirrored) canvas.scale(-1, 1);
          canvas.translate(band.start.x, band.start.y);
          canvas.rotate(SinCos::FromVec2(delta, length).ToDegrees());
          canvas.drawRect(Rect{0, -band.width, length, 0}, paint);
          canvas.restore();
        }

        {  // Lip
          SkPaint ends;
          ends.setColor4f(kGlassTint.withAlpha(0.06f));
          canvas.drawRect(Rect{-kLipHalfWidth, kNeckTop, -kCorkLipHalfWidth, kLipTop}, ends);
          canvas.drawRect(Rect{kCorkLipHalfWidth, kNeckTop, kLipHalfWidth, kLipTop}, ends);

          SkPoint top_pts[] = {Vec2(-kLipHalfWidth, 0), Vec2(kLipHalfWidth, 0)};
          SkColor4f top_colors[] = {SkColors::kWhite.withAlpha(0.6f),
                                    SkColors::kWhite.withAlpha(0.35f),
                                    SkColors::kWhite.withAlpha(0.1f)};
          float top_positions[] = {0.05f, 0.45f, 1};
          SkPaint top_line;
          top_line.setShader(SkShaders::LinearGradient(
              top_pts,
              SkGradient{SkGradient::Colors{top_colors, top_positions, SkTileMode::kClamp}, {}}));
          top_line.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 0.04_mm));
          canvas.drawRect(Rect{-kLipHalfWidth, kLipTop - 0.28_mm, kLipHalfWidth, kLipTop - 0.14_mm},
                          top_line);

          SkPaint underside;
          underside.setColor4f(kGlassEdge.withAlpha(0.45f));
          underside.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 0.04_mm));
          canvas.drawRect(Rect{-kLipHalfWidth, kNeckTop, kLipHalfWidth, kNeckTop + 0.14_mm},
                          underside);

          SkPaint end_light;
          end_light.setStyle(SkPaint::kStroke_Style);
          end_light.setStrokeWidth(0.1_mm);
          end_light.setColor4f(SkColors::kWhite.withAlpha(0.65f));
          end_light.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 0.03_mm));
          SkPaint end_shadow = end_light;
          end_shadow.setColor4f(kGlassEdge.withAlpha(0.35f));
          SkPaint spot;
          spot.setColor4f(SkColors::kWhite.withAlpha(0.6f));
          spot.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 0.06_mm));
          for (float side : {-1.f, 1.f}) {
            Vec2 center = {side * (kLipHalfWidth - kLipRadius), kNeckTop + kLipRadius};
            Rect arc = Rect::MakeCenter(center, 2 * kLipRadius - 0.2_mm, 2 * kLipRadius - 0.2_mm);
            canvas.drawArc(arc, 95, 90, false, end_light);
            canvas.drawArc(arc, 275, 90, false, end_shadow);
            canvas.drawOval(Rect::MakeCenter(center + Vec2(-0.15_mm, 0.15_mm), 0.3_mm, 0.18_mm),
                            spot);
          }
        }

        struct {
          Rect bounds;
          SkColor4f color;
        } lobes[] = {
            {Rect::MakeCenter({-1_mm, kInsideBounds.bottom - 0.1_mm}, 19_mm, 1.2_mm),
             kLiquidLight.withAlpha(0.55f)},
            {Rect::MakeCenter({-2.5_mm, kBottom + 0.25_mm}, 11_mm, 0.5_mm),
             SkColors::kWhite.withAlpha(0.5f)},
        };
        for (auto& lobe : lobes) {
          SkColor4f lobe_colors[] = {lobe.color, lobe.color.withAlpha(0)};
          SkMatrix lobe_matrix = SkMatrix::RectToRect(Rect::MakeCircleR(1), lobe.bounds);
          SkPaint paint;
          paint.setShader(SkShaders::RadialGradient(
              {0, 0}, 1, SkGradient{SkGradient::Colors{lobe_colors, SkTileMode::kClamp}, {}},
              &lobe_matrix));
          canvas.drawOval(lobe.bounds, paint);
        }

        SkPaint inside_edge;
        inside_edge.setStyle(SkPaint::kStroke_Style);
        inside_edge.setStrokeWidth(0.15_mm);
        inside_edge.setColor4f(kGlassEdge.withAlpha(0.3f));
        inside_edge.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 0.08_mm));
        canvas.save();
        canvas.clipRect(Rect{kGlassBounds.left, kBottom, kGlassBounds.right, kBottom + 3_mm});
        canvas.drawPath(inside, inside_edge);
        canvas.restore();
        return SkPath();
      });
      canvas.restore();

      static const SkPaint edge = [] {
        SkPoint pts[] = {kGlassBounds.TopLeftCorner(), kGlassBounds.BottomRightCorner()};
        SkColor4f colors[] = {kGlassEdge.withAlpha(0.15f), kGlassEdge.withAlpha(0.22f),
                              kGlassEdge.withAlpha(0.28f)};
        float positions[] = {0.2f, 0.55f, 0.9f};
        SkPaint paint;
        paint.setAntiAlias(true);
        paint.setStyle(SkPaint::kStroke_Style);
        paint.setStrokeWidth(0.12_mm);
        paint.setShader(SkShaders::LinearGradient(
            pts, SkGradient{SkGradient::Colors{colors, positions, SkTileMode::kClamp}, {}}));
        return paint;
      }();
      DrawStroke(glass, edge);

      static const SkPath edge_light_path = [] {
        constexpr float kEdgeLightInset = 0.22_mm;
        ArcLine outline({-(kNeckHalfWidth - kEdgeLightInset), kNeckTop}, -90_deg);
        AddGlassBody(outline, kNeckTop, kEdgeLightInset);
        return outline.ToPath(false);
      }();
      static const SkPaint edge_light = [] {
        SkColor4f colors[] = {SkColors::kWhite.withAlpha(0.12f), SkColors::kWhite.withAlpha(0.15f),
                              SkColors::kWhite.withAlpha(0.2f),  SkColors::kWhite.withAlpha(0.72f),
                              SkColors::kWhite.withAlpha(0.65f), SkColors::kWhite.withAlpha(0.25f),
                              SkColors::kWhite.withAlpha(0),     SkColors::kWhite.withAlpha(0),
                              SkColors::kWhite.withAlpha(0.18f), SkColors::kWhite.withAlpha(0.12f)};
        float positions[] = {0, 0.2f, 0.29f, 0.34f, 0.4f, 0.55f, 0.62f, 0.83f, 0.9f, 1};
        SkPaint paint;
        paint.setAntiAlias(true);
        paint.setStyle(SkPaint::kStroke_Style);
        paint.setStrokeWidth(0.12_mm);
        paint.setShader(SkShaders::SweepGradient(
            Vec2(0, -4_mm),
            SkGradient{SkGradient::Colors{colors, positions, SkTileMode::kClamp}, {}}));
        return paint;
      }();
      DrawStroke(edge_light_path, edge_light);

      static const SkPaint inside_light = [] {
        SkPoint pts[] = {kGlassBounds.LeftCenter(), kGlassBounds.RightCenter()};
        SkColor4f colors[] = {SkColors::kWhite.withAlpha(0), SkColors::kWhite.withAlpha(0),
                              SkColors::kWhite.withAlpha(0.3f)};
        float positions[] = {0, 0.6f, 0.95f};
        SkPaint paint;
        paint.setAntiAlias(true);
        paint.setStyle(SkPaint::kStroke_Style);
        paint.setStrokeWidth(0.1_mm);
        paint.setShader(SkShaders::LinearGradient(
            pts, SkGradient{SkGradient::Colors{colors, positions, SkTileMode::kClamp}, {}}));
        return paint;
      }();
      canvas.save();
      canvas.clipRect(Rect{kGlassBounds.left, kBottom, kGlassBounds.right, kBottom + 3_mm});
      DrawStroke(inside, inside_light);
      canvas.restore();
    }
  }

  void FillMenu(ui::Pointer& pointer, Menu& menu) override {
    using enum ui::Dir;
    ObjectToy::FillMenu(pointer, menu);
    auto shelf = LockOwner();
    if (!shelf) return;
    menu.Place(W, Proto("Program Launcher"));
    menu.Place(NW, Proto("App Window"));
    menu.Place(SW, Proto("File"));
    menu.Place(NE, Proto("Crop View"));
    menu.Place(N, Interface(*shelf, kPipelinesMenu));
  }
};

const BetaShelfToy::Liquid& BetaShelfToy::SettledLiquid() {
  static const Liquid settled = [] {
    Liquid liquid = {};
    int count = 0;
    for (int row = 0; count < kParticles; ++row) {
      float y = kInsideBounds.bottom + kWallDistance + row * kLatticeSpacing * sqrtf(3) / 2;
      for (float x = kInsideBounds.left + (row % 2) * kLatticeSpacing / 2;
           x < kInsideBounds.right && count < kParticles; x += kLatticeSpacing) {
        Vec2 inward;
        if (DistanceToWall({x, y}, inward) < kWallDistance) continue;
        liquid.position[count] = {x, y};
        ++count;
      }
    }
    liquid.Simulate(kSettleTime, {});
    return liquid;
  }();
  return settled;
}

animation::Progress BetaShelfToy::Liquid::Simulate(float delta_time, Vec2 target_flask_velocity) {
  unsimulated_time += delta_time;
  bool moved = false;
  float drag = expf(-kStepTime / kDragTime);
  float sheen_rise = -expm1f(-kStepTime / kSheenRiseTime);
  float sheen_fade = expf(-kStepTime / kSheenFadeTime);
  while (unsimulated_time >= kStepTime) {
    unsimulated_time -= kStepTime;
    moved = true;
    Vec2 previous[kParticles];
    {  // Movement
      Vec2 previous_flask_velocity = flask_velocity;
      animation::ExponentialApproach(target_flask_velocity.x, kStepTime, kFollowTime,
                                     flask_velocity.x);
      animation::ExponentialApproach(target_flask_velocity.y, kStepTime, kFollowTime,
                                     flask_velocity.y);
      Vec2 flask_acceleration = (flask_velocity - previous_flask_velocity) / kStepTime;
      Vec2 acceleration =
          kGravity -
          flask_acceleration *
              (kFlaskAccelerationScale /
               (1 + kFlaskAccelerationScale * Length(flask_acceleration) / kMaxLiquidAcceleration));
      for (int i = 0; i < kParticles; ++i) {
        velocity[i] = LimitLength((velocity[i] + acceleration * kStepTime) * drag, kMaxSpeed);
        previous[i] = position[i];
        position[i] += velocity[i] * kStepTime;
      }
    }
    static_assert(kParticles <= 256);
    uint8_t pairs[kMaxPairs][2];
    int pair_count = 0;
    {  // Pairs
      constexpr int kColumns = (int)(kInsideBounds.Width() / kInteractionRadius) + 1;
      constexpr int kRows = (int)(kInsideBounds.Height() / kInteractionRadius) + 1;
      int cell[kParticles];
      int cell_end[kColumns * kRows] = {};
      for (int i = 0; i < kParticles; ++i) {
        int column = std::clamp((int)((position[i].x - kInsideBounds.left) / kInteractionRadius), 0,
                                kColumns - 1);
        int row = std::clamp((int)((position[i].y - kInsideBounds.bottom) / kInteractionRadius), 0,
                             kRows - 1);
        cell[i] = row * kColumns + column;
        ++cell_end[cell[i]];
      }
      for (int c = 1; c < kColumns * kRows; ++c) cell_end[c] += cell_end[c - 1];
      uint8_t sorted[kParticles];
      int fill[kColumns * kRows];
      for (int c = 0; c < kColumns * kRows; ++c) fill[c] = c ? cell_end[c - 1] : 0;
      for (int i = 0; i < kParticles; ++i) sorted[fill[cell[i]]++] = i;
      for (int i = 0; i < kParticles; ++i) {
        int column = cell[i] % kColumns, row = cell[i] / kColumns;
        for (int r = row; r <= std::min(row + 1, kRows - 1); ++r) {
          int first = r * kColumns + std::max(column - (r > row), 0);
          int last = r * kColumns + std::min(column + 1, kColumns - 1);
          for (int k = first ? cell_end[first - 1] : 0; k < cell_end[last]; ++k) {
            int j = sorted[k];
            if ((cell[j] == cell[i] && j <= i) || pair_count == kMaxPairs) continue;
            if (LengthSquared(position[j] - position[i]) <
                kInteractionRadius * kInteractionRadius) {
              pairs[pair_count][0] = i;
              pairs[pair_count][1] = j;
              ++pair_count;
            }
          }
        }
      }
    }
    {  // Double density relaxation
      for (int iteration = 0; iteration < kRelaxationIterations; ++iteration) {
        float density[kParticles] = {};
        float near_density[kParticles] = {};
        float distance[kMaxPairs];
        for (int p = 0; p < pair_count; ++p) {
          int i = pairs[p][0], j = pairs[p][1];
          distance[p] = Length(position[j] - position[i]);
          float q = distance[p] / kInteractionRadius;
          if (q >= 1) continue;
          float w = (1 - q) * (1 - q);
          density[i] += w;
          density[j] += w;
          near_density[i] += w * (1 - q);
          near_density[j] += w * (1 - q);
        }
        float pressure[kParticles], near_pressure[kParticles];
        for (int i = 0; i < kParticles; ++i) {
          pressure[i] = std::max(kStiffness * (density[i] - kReferenceDensity), kMinPressure);
          near_pressure[i] = kNearStiffness * near_density[i];
        }
        Vec2 displacement[kParticles] = {};
        for (int p = 0; p < pair_count; ++p) {
          int i = pairs[p][0], j = pairs[p][1];
          float q = distance[p] / kInteractionRadius;
          if (q >= 1 || distance[p] == 0) continue;
          float push = kStepTime * kStepTime *
                       ((pressure[i] + pressure[j]) * (1 - q) +
                        (near_pressure[i] + near_pressure[j]) * (1 - q) * (1 - q)) /
                       2;
          Vec2 half = (position[j] - position[i]) * (push / distance[p] / 2);
          displacement[i] -= half;
          displacement[j] += half;
        }
        for (int i = 0; i < kParticles; ++i) {
          position[i] += LimitLength(displacement[i], kMaxDisplacement);
        }
      }
    }
    {  // Walls
      for (int i = 0; i < kParticles; ++i) {
        Vec2 first_inward;
        float distance = DistanceToWall(position[i], first_inward);
        if (distance >= kWallDistance) continue;
        position[i] += first_inward * (kWallDistance - distance);
        Vec2 second_inward;
        distance = DistanceToWall(position[i], second_inward);
        if (distance >= kWallDistance) continue;
        float inward_sine = Cross(first_inward, second_inward);
        if (fabsf(inward_sine) < kParallelWallSine) {
          position[i] += second_inward * (kWallDistance - distance);
          continue;
        }
        float first_line_offset = Dot(first_inward, position[i]);
        float second_line_offset = Dot(second_inward, position[i]) + kWallDistance - distance;
        position[i] =
            Vec2(first_line_offset * second_inward.y - second_line_offset * first_inward.y,
                 second_line_offset * first_inward.x - first_line_offset * second_inward.x) /
            inward_sine;
      }
    }
    float shear[kParticles] = {};
    float weight[kParticles] = {};
    {  // Viscosity
      for (int i = 0; i < kParticles; ++i) velocity[i] = (position[i] - previous[i]) / kStepTime;
      Vec2 change[kParticles] = {};
      for (int p = 0; p < pair_count; ++p) {
        int i = pairs[p][0], j = pairs[p][1];
        Vec2 offset = position[j] - position[i];
        float distance = Length(offset);
        float q = distance / kInteractionRadius;
        if (q >= 1 || distance == 0) continue;
        Vec2 relative = velocity[i] - velocity[j];
        shear[i] += (1 - q) * LengthSquared(relative);
        shear[j] += (1 - q) * LengthSquared(relative);
        weight[i] += 1 - q;
        weight[j] += 1 - q;
        float approach = std::max(Dot(relative, offset) / distance, 0.f);
        Vec2 impulse =
            offset * (kStepTime * (1 - q) * kLinearViscosity * approach / (2 * distance));
        change[i] -= impulse;
        change[j] += impulse;
      }
      for (int i = 0; i < kParticles; ++i) velocity[i] += change[i];
    }
    {  // Sheen
      for (int i = 0; i < kParticles; ++i) {
        float target =
            weight[i] > 0 ? std::min(1.f, shear[i] / weight[i] / (kSheenShear * kSheenShear)) : 0;
        if (target > sheen[i]) {
          sheen[i] += (target - sheen[i]) * sheen_rise;
        } else {
          sheen[i] *= sheen_fade;
        }
      }
    }
  }
  float max_speed_squared = 0;
  for (int i = 0; i < kParticles; ++i) {
    max_speed_squared = std::max(max_speed_squared, LengthSquared(velocity[i]));
  }
  return {.value_changed = moved, .settled = max_speed_squared < kSettledSpeed * kSettledSpeed};
}

float BetaShelfToy::Liquid::Level() const {
  constexpr int kTopParticles = 24;
  float top[kTopParticles];
  for (int k = 0; k < kTopParticles; ++k) top[k] = -INFINITY;
  for (int i = 0; i < kParticles; ++i) {
    float y = position[i].y;
    for (int k = 0; k < kTopParticles; ++k) {
      if (y > top[k]) std::swap(y, top[k]);
    }
  }
  float sum = 0;
  for (int k = 0; k < kTopParticles; ++k) sum += top[k];
  return sum / kTopParticles + kSurfaceDistance;
}

constinit Interface::Table kFfmpegMenu = MenuTable("FFmpeg", &FillFfmpegMenu);
constinit Interface::Table kTensorFlowMenu = MenuTable("TensorFlow", &FillTensorFlowMenu);
constinit Interface::Table kPipelinesMenu = MenuTable("Pipelines", &FillPipelinesMenu);

std::unique_ptr<ObjectToy> BetaShelf::MakeToy(ui::Widget* parent) {
  return std::make_unique<BetaShelfToy>(parent, *this);
}

}  // namespace automat::library
