// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "library_flip_flop.hpp"

#include <include/core/SkBlendMode.h>
#include <include/core/SkBlurTypes.h>
#include <include/core/SkColor.h>
#include <include/core/SkMaskFilter.h>
#include <include/core/SkPaint.h>
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

  void Draw(SkCanvas& canvas) const override {
    SkPaint border_paint;
    SetRRectShader(border_paint, kBounds, kBorderLight, kBorderSide, kBorderDark);
    canvas.drawRRect(kBounds, border_paint);

    auto& flat = kFlatRRect.rect;
    SkPaint flat_paint;
    SkPoint flat_pts[] = {flat.TopCenter(), flat.BottomCenter()};
    SkColor4f flat_colors[] = {kBgLight, kBgDark};
    flat_paint.setShader(SkShaders::LinearGradient(
        flat_pts, SkGradient{SkGradient::Colors{flat_colors, SkTileMode::kClamp}, {}}));

    flat_paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 1_mm));
    canvas.drawRRect(kFlatRRect, flat_paint);

    SkPaint socket_paint;
    SetRRectShader(socket_paint, kSocketRRect, kBorderLight, kBorderSide, kBorderDark);
    socket_paint.setMaskFilter(SkMaskFilter::MakeBlur(kOuter_SkBlurStyle, 2_mm));
    canvas.drawRRect(kSocketRRect, socket_paint);

    {  // Red indicator light
      SkPoint center = {0, (flat.top + kSocketRRect.rect.top) / 2};
      float radius = 2_mm;

      SkPaint inset_paint;
      SkColor4f inset_colors[] = {
          kBorderSide,  kBorderDark,  kBorderDark, kBorderSide,
          kBorderLight, kBorderLight, kBorderSide,
      };
      inset_paint.setShader(SkShaders::SweepGradient(
          center, SkGradient{SkGradient::Colors{inset_colors, SkTileMode::kClamp}, {}}));
      inset_paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, radius));
      canvas.drawCircle(center, radius * 1.5, inset_paint);

      float a = light;
      SkColor4f gradient_colors[] = {color::MixColors("#725016"_color4f, "#ff8786"_color4f, a),
                                     color::MixColors("#2b1e07"_color4f, "#ff3e3e"_color4f, a)};
      SkPaint gradient;
      gradient.setShader(SkShaders::RadialGradient(
          center + SkPoint(0, radius * 0.25), radius,
          SkGradient{SkGradient::Colors{gradient_colors, SkTileMode::kClamp}, {}}));
      canvas.drawCircle(center, radius, gradient);

      SkPaint shine;
      SkColor4f shine_colors[] = {color::MixColors("#d2b788ff"_color4f, "#ffe8e8ff"_color4f, a),
                                  color::MixColors("#d2b78800"_color4f, "#ffe8e800"_color4f, a),
                                  color::MixColors("#d2b788ff"_color4f, "#ffe8e8ff"_color4f, a)};
      SkPoint shine_pts[] = {center + SkPoint{0, radius}, center - SkPoint{0, radius}};
      shine.setShader(SkShaders::LinearGradient(
          shine_pts, SkGradient{SkGradient::Colors{shine_colors, SkTileMode::kClamp}, {}}));
      shine.setStyle(SkPaint::kStroke_Style);
      shine.setStrokeWidth(radius / 9);
      shine.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, radius / 10));
      canvas.drawCircle(center, radius * 0.8, shine);

      SkPaint red_glow;
      red_glow.setColor("#ff3e3e"_color);
      red_glow.setAlphaf(a);
      red_glow.setBlendMode(SkBlendMode::kHardLight);
      red_glow.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, radius));
      canvas.drawCircle(center, radius, red_glow);
    }
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
