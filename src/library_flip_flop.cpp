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

FlipFlopController::FlipFlopController(OnOff on_off) : on_off(on_off) {}

string_view FlipFlopController::Name() const { return "Flip-Flop Controller"; }

Ptr<Object> FlipFlopController::Clone() const {
  return MAKE_PTR(FlipFlopController, on_off.Lock());
}

void FlipFlopController::SerializeState(ObjectSerializer& writer) const {
  if (auto locked = on_off.Lock()) {
    writer.Key("on_off");
    writer.String(writer.ResolveName(*locked.object_ptr, locked.table_ptr));
  }
}
bool FlipFlopController::DeserializeKey(ObjectDeserializer& d, StrView key) {
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

struct FlipFlopWidgetBase : ObjectToy {
  MiniMenuMode MenuMode() override { return MODE_4_DIR; }
  float light = 0;
  bool current_state = false;
  std::unique_ptr<ui::Rocker> rocker;
  animation::SpringV2<float> iconification;

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

  FlipFlopWidgetBase(ui::Widget* parent, Object& object, OnOff on_off) : ObjectToy(parent, object) {
    rocker = std::make_unique<ui::Rocker>(this);
    rocker->target = on_off;
  }

  float GetBaseScale() const override { return 1.0f; }

  RRect CoarseBounds() const override { return Lerp(kFullBounds, kIconBounds, iconification); }

  Tock Tick(time::Timer& timer) override {
    Tock tock;
    tock.drawing |= animation::LinearApproach(current_state, timer.d, 10, light);
    if (last_tick == time::SteadyPoint::min()) {
      iconification = iconified ? 1.f : 0.f;
    }
    tock.shaping |= iconification.SineTowards(iconified ? 1.f : 0.f, timer.d, 0.4);
    rocker->SetOn(current_state);
    rocker->alpha = 1 - iconification;
    return tock;
  }

  static inline RasterPatch bg_patch;
  static inline RasterPatch ic_patch;
  static inline RasterPatch light_off_patch;
  static inline RasterPatch light_on_patch;
  static inline RasterPatch light_glow_patch;

  virtual StrView Label() const = 0;

  void Draw(SkCanvas& canvas) const override {
    auto bounds = CoarseBounds();
    auto& flat = kFlatRRect.rect;
    float expansion = 1.f - iconification;
    SkPoint light_center = {0, (flat.top + kSocketRRect.rect.top) / 2};
    SkPoint ic_light_center = {0, 0_mm};
    auto ic_light_vec = ic_light_center - light_center;
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

            SkPathBuilder clip_builder(shape);
            clip_builder.addRRect(kSocketRRect);
            clip_builder.setFillType(SkPathFillType::kEvenOdd);

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
    auto label = Label();
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

struct FlipFlopWidget : FlipFlopWidgetBase {
  FlipFlopWidget(ui::Widget* parent, FlipFlop& object)
      : FlipFlopWidgetBase(parent, object, object.enabled.Bind()) {}

  Interface FindOption(ui::Pointer& pointer, ui::ActionTrigger trigger) override {
    using enum ui::Dir;
    auto object = LockObject<FlipFlop>();
    if (!object) return {};
    switch (static_cast<ui::Dir>(trigger)) {
      case E:
        return object->enabled.Bind();
      default:
        return ObjectToy::FindOption(pointer, trigger);
    }
  }

  StrView Label() const override { return "On/Off"sv; }

  Tock Tick(time::Timer& timer) override {
    if (auto ptr = LockObject<FlipFlop>()) {
      current_state = ptr->current_state;
    } else {
      MarkDead(timer.now);
    }
    return FlipFlopWidgetBase::Tick(timer);
  }
};

struct FlipFlopControllerWidget : FlipFlopWidgetBase {
  FlipFlopControllerWidget(ui::Widget* parent, FlipFlopController& object)
      : FlipFlopWidgetBase(parent, object, object.on_off.Lock()) {}

  Interface FindOption(ui::Pointer& pointer, ui::ActionTrigger trigger) override {
    using enum ui::Dir;
    auto object = LockObject<FlipFlopController>();
    if (!object) return {};
    switch (static_cast<ui::Dir>(trigger)) {
      case E:
        return object->on_off.Lock();
      default:
        return ObjectToy::FindOption(pointer, trigger);
    }
  }

  Str label = "?"s;

  StrView Label() const override { return label; }

  Tock Tick(time::Timer& timer) override {
    if (auto ptr = LockObject<FlipFlopController>()) {
      if (auto locked = ptr->on_off.Lock()) {
        current_state = locked.IsOn();
        label = locked.Name();
      }
    } else {
      MarkDead(timer.now);
    }
    return FlipFlopWidgetBase::Tick(timer);
  }

  uint32_t observed_on_off_monitor;

  void OnPoll(time::Timer& timer) override {
    if (IsAnimating()) return;
    uint32_t current = rocker->target.Unsafe().object_ptr->monitor.load(std::memory_order_relaxed);
    if (current != observed_on_off_monitor) {
      observed_on_off_monitor = current;
      WakeAnimationAt(timer.last);
      OnWake();
    } else if (rocker->target.IsExpired()) {
      rocker->target.Reset();
    }
    FlipFlopWidgetBase::OnPoll(timer);
  }
};

std::unique_ptr<ObjectToy> FlipFlopController::MakeToy(ui::Widget* parent) {
  return std::make_unique<FlipFlopControllerWidget>(parent, *this);
}

std::unique_ptr<ObjectToy> FlipFlop::MakeToy(ui::Widget* parent) {
  return std::make_unique<FlipFlopWidget>(parent, *this);
}
}  // namespace automat::library
