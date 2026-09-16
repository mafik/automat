// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "library_switch.hpp"

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

LinkedSwitch::LinkedSwitch(OnOff on_off) : on_off(on_off) {}

string_view LinkedSwitch::Name() const { return "Linked Switch"; }

Ptr<Object> LinkedSwitch::Clone() const { return MAKE_PTR(LinkedSwitch, on_off.Lock()); }

void LinkedSwitch::SerializeState(ObjectSerializer& writer) const {
  if (auto locked = on_off.Lock()) {
    writer.Key("on_off");
    writer.String(writer.ResolveName(*locked.object_ptr, locked.table_ptr));
  }
}
bool LinkedSwitch::DeserializeKey(ObjectDeserializer& d, StrView key) {
  if (key == "on_off") {
    Str name;
    Status status;
    d.Get(name, status);
    if (!OK(status)) {
      ReportError(status.ToStr());
    } else {
      on_off = cast<OnOff>(d.LookupInterface(name));
    }
    return true;
  }
  return false;
}

string_view Switch::Name() const { return "Switch"; }

Ptr<Object> Switch::Clone() const {
  auto ret = MAKE_PTR(Switch);
  ret->current_state = current_state;
  return ret;
}

void Switch::SerializeState(ObjectSerializer& writer) const {
  writer.Key("on");
  writer.Bool(current_state);
}

bool Switch::DeserializeKey(ObjectDeserializer& d, StrView key) {
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

struct SwitchToy : ObjectToy {
  MiniMenuMode MenuMode() override { return MODE_4_DIR; }
  std::unique_ptr<ui::Rocker> rocker;
  Str label = "?"s;
  float light = 0;
  animation::SpringV2<float> iconification;
  uint32_t observed_target_monitor = 0;

  constexpr static SkColor4f kBgLight = "#216778"_color4f;
  constexpr static SkColor4f kBgDark = "#164450"_color4f;
  constexpr static SkColor4f kBorderLight = "#6ea7a7"_color4f;
  constexpr static SkColor4f kBorderSide = "#386f7d"_color4f;
  constexpr static SkColor4f kBorderDark = "#00363f"_color4f;

  constexpr static auto kFullBounds = RRect::MakeSimple(Rect::MakeCenterZero(3_cm, 5_cm), 5_mm);
  constexpr static auto kIconBounds = RRect::MakeSimple(Rect(-5_mm, -5_mm, 5_mm, 5_mm), 1_mm);

  constexpr static auto kFlatRRect = kFullBounds.Outset(-2_mm);
  // The raised part of the panel that the rocker is mounted in.
  constexpr static auto kSocketRRect = ui::Rocker::kBounds.Outset(1_mm);

  SwitchToy(ui::Widget* parent, Object& object, Linked<OnOff> on_off)
      : ObjectToy(parent, object), rocker(std::make_unique<ui::Rocker>(this)) {
    rocker->target = on_off;
  }

  Interface FindOption(ui::Pointer& pointer, ui::ActionTrigger trigger) override {
    if (trigger == ui::Dir::E) return rocker->target.Lock();
    return ObjectToy::FindOption(pointer, trigger);
  }

  float GetBaseScale() const override { return 1.0f; }

  RRect CoarseBounds() const override { return Lerp(kFullBounds, kIconBounds, iconification); }

  Tock Tick(time::Timer& timer) override {
    if (owner.IsExpired()) MarkDead(timer.now);
    if (auto on_off = rocker->target.Lock()) {
      rocker->SetOn(on_off.IsOn());
      label = on_off.Name();
    }
    Tock tock;
    tock.drawing |= animation::LinearApproach(rocker->on, timer.d, 10, light);
    if (last_tick == time::SteadyPoint::min()) {
      iconification = iconified ? 1.f : 0.f;
    }
    tock.shaping |= iconification.SineTowards(iconified ? 1.f : 0.f, timer.d, 0.4);
    rocker->alpha = 1 - iconification;
    return tock;
  }

  void OnPoll(time::Timer& timer) override {
    auto target = rocker->target.Unsafe();
    if (!target) return;
    uint32_t current = target.object_ptr->monitor.load(std::memory_order_relaxed);
    if (current != observed_target_monitor) {
      observed_target_monitor = current;
      WakeAnimationAt(timer.last);
    }
  }

  static inline RasterPatch bg_patch;
  static inline RasterPatch ic_patch;
  static inline RasterPatch light_off_patch;
  static inline RasterPatch light_on_patch;
  static inline RasterPatch light_glow_patch;

  void Draw(SkCanvas& canvas) const override {
    auto bounds = CoarseBounds();
    auto& flat = kFlatRRect.rect;
    SkPoint light_center = {0, (flat.top + kSocketRRect.rect.top) / 2};
    SkPoint ic_light_center = {0, 0_mm};
    float light_radius = 2_mm;

    SkPaint flat_paint;
    SkPoint flat_pts[] = {flat.TopCenter(), flat.BottomCenter()};
    SkColor4f flat_colors[] = {kBgLight, kBgDark};
    flat_paint.setShader(SkShaders::LinearGradient(
        flat_pts, SkGradient{SkGradient::Colors{flat_colors, SkTileMode::kClamp}, {}}));

    if (iconification < 1.0f) {  // Draw PLATE background
      if (iconification > 0.f) {
        canvas.save();
        canvas.scale(bounds.rect.Width() / kFullBounds.rect.Width(),
                     bounds.rect.Height() / kFullBounds.rect.Height());
      }
      SkMatrix pxToLocal;
      (void)canvas.getTotalMatrix().invert(&pxToLocal);
      canvas.drawRect(kSocketRRect.rect.Outset(pxToLocal.mapRadius(1)), flat_paint);
      bg_patch.DrawCached(canvas, kFullBounds.rect, SkISize(18, 30), [&](SkCanvas& canvas) {
        SkPaint border_paint;
        SetRRectShader(border_paint, kFullBounds, kBorderLight, kBorderSide, kBorderDark);
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
      if (iconification > 0.f) {
        canvas.restore();
      }
    }

    if (iconification > 0.0f) {  // Draw ICON background
      if (iconification < 1.0f) {
        canvas.save();
        canvas.scale(bounds.rect.Width() / kIconBounds.rect.Width(),
                     bounds.rect.Height() / kIconBounds.rect.Height());
      }
      SkPaint ic_paint;
      ic_paint.setAlphaf(iconification);
      ic_patch.DrawCached(
          canvas, kIconBounds.rect, SkISize(16, 16),
          [&](SkCanvas& canvas) {
            SkPaint border_paint;
            SetRRectShader(border_paint, kIconBounds, kBorderLight, kBorderSide, kBorderDark);
            canvas.drawPaint(border_paint);

            SkPaint blurry_flat_paint = flat_paint;
            blurry_flat_paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 0.5_mm));
            canvas.drawRRect(kIconBounds.Outset(-0.5_mm), blurry_flat_paint);

            SkPaint inset_paint;
            SkColor4f inset_colors[] = {
                kBorderSide,  kBorderDark,  kBorderDark, kBorderSide,
                kBorderLight, kBorderLight, kBorderSide,
            };
            inset_paint.setShader(SkShaders::SweepGradient(
                ic_light_center,
                SkGradient{SkGradient::Colors{inset_colors, SkTileMode::kClamp}, {}}));
            inset_paint.setMaskFilter(
                SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, light_radius / 3));
            canvas.drawCircle(ic_light_center, light_radius * 1.3, inset_paint);

            return SkPath::RRect(kIconBounds);
          },
          &ic_paint);
      if (iconification < 1.0f) {
        canvas.restore();
      }
    }

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

    if (iconification > 0) {
      canvas.save();
      canvas.translate(0, (ic_light_center.y() - light_center.y()) * iconification);
    }

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

    if (iconification > 0) {
      canvas.restore();
    }

    static auto font = ui::Font::MakeV2(ui::Font::GetHelsinki(), 4_mm);
    SkPaint label_paint;
    label_paint.setColor(SK_ColorWHITE);
    label_paint.setAlphaf(0.9f);
    auto label_w = font->MeasureText(label);
    // TODO: text sizing is kind of crappy now as very short text could cause glitches -  fix it
    float label_scale = min(1.0f, bounds.rect.Width() * 0.9f / label_w);
    canvas.save();
    float y_full =
        (flat.bottom + kSocketRRect.rect.bottom) / 2 - font->letter_height * label_scale / 2;
    float y_ic = -4_mm / label_scale;
    canvas.scale(label_scale, label_scale);
    canvas.translate(-label_w / 2, std::lerp(y_full, y_ic, iconification));
    font->DrawText(canvas, label, label_paint);
    canvas.restore();
  }
  SkPath Shape() const override { return SkPath::RRect(CoarseBounds()); }
  bool CenteredAtZero() const override { return true; }
};

std::unique_ptr<ObjectToy> LinkedSwitch::MakeToy(ui::Widget* parent) {
  return std::make_unique<SwitchToy>(parent, *this, on_off);
}

std::unique_ptr<ObjectToy> Switch::MakeToy(ui::Widget* parent) {
  return std::make_unique<SwitchToy>(parent, *this, enabled.Bind());
}
}  // namespace automat::library
