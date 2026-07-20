// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "library_flip_flop.hpp"

#include <include/core/SkBlendMode.h>
#include <include/core/SkBlurTypes.h>
#include <include/core/SkColor.h>
#include <include/core/SkMaskFilter.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPathBuilder.h>
#include <include/core/SkPathTypes.h>
#include <include/core/SkShader.h>
#include <include/core/SkTileMode.h>
#include <include/effects/SkGradient.h>

#include <memory>

#include "animation.hpp"
#include "color.hpp"
#include "drawing.hpp"
#include "font.hpp"
#include "math.hpp"
#include "ui_rocker.hpp"
#include "widget.hpp"

using namespace std;

namespace automat::library {

string_view FlipFlop::Name() const { return "Flip-Flop"; }

Ptr<Object> FlipFlop::Clone() const {
  auto ret = MAKE_PTR(FlipFlop);
  ret->current_state = current_state;
  return ret;
}

void FlipFlop::SerializeState(ObjectSerializer& writer) const {
  writer.Key("on");
  writer.Bool(current_state);
}

bool FlipFlop::DeserializeKey(ObjectDeserializer& d, StrView key) {
  if (key == "on") {
    Status status;
    d.Get(current_state, status);
    if (!OK(status)) {
      ReportError(status.ToStr());
    }
    return true;
  }
  return false;
}

struct FlipFlopWidget : ObjectToy {
  float light = 0;
  bool current_state = false;
  std::unique_ptr<ui::Rocker> rocker;

  constexpr static SkColor4f kBgLight = "#216778"_color4f;
  constexpr static SkColor4f kBgDark = "#164450"_color4f;
  constexpr static SkColor4f kBorderLight = "#6ea7a7"_color4f;
  constexpr static SkColor4f kBorderSide = "#386f7d"_color4f;
  constexpr static SkColor4f kBorderDark = "#00363f"_color4f;

  constexpr static auto kBounds = RRect::MakeSimple(Rect::MakeCenterZero(3_cm, 5_cm), 5_mm);
  constexpr static auto kFlatRRect = kBounds.Outset(-2_mm);
  // The raised part of the panel that the rocker is mounted in.
  constexpr static auto kSocketRRect = ui::Rocker::kBounds.Outset(1_mm);

  FlipFlopWidget(ui::Widget* parent, Object& object) : ObjectToy(parent, object) {
    rocker = std::make_unique<ui::Rocker>(this);
    rocker->clickable.activate = [this](ui::Pointer&) {
      if (auto ptr = LockObject<FlipFlop>()) {
        ptr->enabled->Toggle();
      }
    };
  }

  RRect CoarseBounds() const override { return kBounds; }

  ui::Widget* FindWidget(Interface::Table* iface) override {
    if (iface == &FlipFlop::enabled_tbl) return rocker.get();
    return this;
  }

  Tock Tick(time::Timer& timer) override {
    if (auto ptr = LockObject<FlipFlop>()) {
      current_state = ptr->current_state;
    }
    rocker->SetOn(current_state);
    Tock tock;
    tock.drawing |= animation::LinearApproach(current_state, timer.d, 10, light);
    return tock;
  }

  static inline RasterPatch bg_patch;
  static inline RasterPatch light_off_patch;
  static inline RasterPatch light_on_patch;
  static inline RasterPatch light_glow_patch;

  void Draw(SkCanvas& canvas) const override {
    auto& flat = kFlatRRect.rect;
    SkPoint light_center = {0, (flat.top + kSocketRRect.rect.top) / 2};
    float light_radius = 2_mm;

    SkPaint flat_paint;
    SkPoint flat_pts[] = {flat.TopCenter(), flat.BottomCenter()};
    SkColor4f flat_colors[] = {kBgLight, kBgDark};
    flat_paint.setShader(SkShaders::LinearGradient(
        flat_pts, SkGradient{SkGradient::Colors{flat_colors, SkTileMode::kClamp}, {}}));
    canvas.drawRect(kSocketRRect.rect, flat_paint);

    bg_patch.DrawCached(canvas, kBounds.rect, SkISize(18, 30), [&](SkCanvas& canvas) {
      SkPaint border_paint;
      SetRRectShader(border_paint, kBounds, kBorderLight, kBorderSide, kBorderDark);
      canvas.drawPaint(border_paint);

      SkPaint blurry_flat_paint = flat_paint;
      blurry_flat_paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 1_mm));
      canvas.drawRRect(kFlatRRect, blurry_flat_paint);

      SkPaint socket_paint;
      SetRRectShader(socket_paint, kSocketRRect, kBorderLight, kBorderSide, kBorderDark);
      socket_paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 2_mm));
      canvas.drawRRect(kSocketRRect, socket_paint);

      SkPaint inset_paint;
      SkColor4f inset_colors[] = {
          kBorderSide,  kBorderDark,  kBorderDark, kBorderSide,
          kBorderLight, kBorderLight, kBorderSide,
      };
      inset_paint.setShader(SkShaders::SweepGradient(
          light_center, SkGradient{SkGradient::Colors{inset_colors, SkTileMode::kClamp}, {}}));
      inset_paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, light_radius));
      canvas.drawCircle(light_center, light_radius * 1.5, inset_paint);

      SkPathBuilder clip_builder(shape);
      clip_builder.addRRect(kSocketRRect);
      clip_builder.setFillType(SkPathFillType::kEvenOdd);

      return clip_builder.detach();
    });

    auto FillLight = [&](SkCanvas& canvas, float a) {
      SkColor4f gradient_colors[] = {color::MixColors("#725016"_color4f, "#ff8786"_color4f, a),
                                     color::MixColors("#2b1e07"_color4f, "#ff3e3e"_color4f, a)};
      SkPaint gradient;
      gradient.setShader(SkShaders::RadialGradient(
          light_center + SkPoint(0, light_radius * 0.25), light_radius,
          SkGradient{SkGradient::Colors{gradient_colors, SkTileMode::kClamp}, {}}));
      canvas.drawPaint(gradient);

      SkPaint shine;
      SkColor4f shine_colors[] = {color::MixColors("#d2b788ff"_color4f, "#ffe8e8ff"_color4f, a),
                                  color::MixColors("#d2b78800"_color4f, "#ffe8e800"_color4f, a),
                                  color::MixColors("#d2b788ff"_color4f, "#ffe8e8ff"_color4f, a)};
      SkPoint shine_pts[] = {light_center + SkPoint{0, light_radius},
                             light_center - SkPoint{0, light_radius}};
      shine.setShader(SkShaders::LinearGradient(
          shine_pts, SkGradient{SkGradient::Colors{shine_colors, SkTileMode::kClamp}, {}}));
      shine.setStyle(SkPaint::kStroke_Style);
      shine.setStrokeWidth(light_radius / 9);
      shine.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, light_radius / 10));
      canvas.drawCircle(light_center, light_radius * 0.8, shine);
    };

    light_off_patch.DrawCached(
        canvas, Rect::MakeCenter(light_center, light_radius * 2, light_radius * 2), SkISize(32, 32),
        [&](SkCanvas& canvas) {
          FillLight(canvas, 0);
          return SkPath::Circle(light_center.x(), light_center.y(), light_radius);
        });

    SkPaint light_on_paint;
    light_on_paint.setAlphaf(light);
    light_on_patch.DrawCached(
        canvas, Rect::MakeCenter(light_center, light_radius * 2, light_radius * 2), SkISize(24, 24),
        [&](SkCanvas& canvas) {
          FillLight(canvas, 1);
          return SkPath::Circle(light_center.x(), light_center.y(), light_radius);
        },
        &light_on_paint);

    SkPaint light_glow_paint = light_on_paint;
    light_glow_paint.setBlendMode(SkBlendMode::kHardLight);
    light_glow_patch.DrawCached(
        canvas, Rect::MakeCenter(light_center, light_radius * 6, light_radius * 6), SkISize(15, 15),
        [&](SkCanvas& canvas) {
          SkPaint red_glow;
          red_glow.setColor("#ff3e3e"_color);
          red_glow.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, light_radius));
          canvas.drawCircle(light_center, light_radius * 1.5, red_glow);
          return SkPath();
        },
        &light_on_paint);

    static auto font = ui::Font::MakeV2(ui::Font::GetHelsinki(), 4_mm);
    SkPaint label_paint;
    label_paint.setColor(SK_ColorWHITE);
    label_paint.setAlphaf(0.9f);
    auto label = "On/Off"sv;
    auto label_w = font->MeasureText(label);
    canvas.save();
    canvas.translate(-label_w / 2,
                     (flat.bottom + kSocketRRect.rect.bottom) / 2 - font->letter_height / 2);
    font->DrawText(canvas, label, label_paint);
    canvas.restore();
  }
  SkPath Shape() const override { return SkPath::RRect(CoarseBounds()); }
  bool CenteredAtZero() const override { return true; }
};

std::unique_ptr<ObjectToy> FlipFlop::MakeToy(ui::Widget* parent) {
  return std::make_unique<FlipFlopWidget>(parent, *this);
}
}  // namespace automat::library
