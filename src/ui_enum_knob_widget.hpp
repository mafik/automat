#pragma once
// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include <include/core/SkPath.h>

#include "action.hpp"
#include "animation.hpp"
#include "knob.hpp"
#include "math.hpp"
#include "optional.hpp"
#include "pointer.hpp"
#include "ptr.hpp"
#include "time.hpp"
#include "units.hpp"
#include "widget.hpp"

class SkCanvas;

namespace automat::ui {

struct EnumKnobWidget : ui::Widget {
  float last_vx = 0;
  Knob knob;

  constexpr static time::FloatDuration kClickWigglePeriod = 0.5s;
  constexpr static time::FloatDuration kClickWiggleHalfTime = 0.1s;
  animation::SpringV2<float> click_wiggle = {};
  bool is_dragging = false;
  float cond_code_float = 0;
  int n_options;

  int value = 0;  // ground-truth value, obtained from the getter in Tick

  EnumKnobWidget(ui::Widget* parent, int n_options);

  SkPath Shape() const override;
  void TransformUpdated(time::Timer& t) override { WakeAnimationAt(t.now); }

  static constexpr float kBorderWidth = 0.5_mm;
  static constexpr float kBorderHalf = kBorderWidth / 2;
  static constexpr float kGaugeRadius = 4_mm;
  static constexpr Rect kGaugeOval = Rect::MakeAtZero(kGaugeRadius * 2, kGaugeRadius * 2);
  static constexpr float kInnerRadius = kGaugeRadius - kBorderWidth;
  static constexpr Rect kInnerOval = Rect::MakeAtZero(kInnerRadius * 2, kInnerRadius * 2);
  static constexpr float kSymbolRadius = 2_mm;
  static constexpr Rect kSymbolOval = Rect::MakeAtZero(kSymbolRadius * 2, kSymbolRadius * 2);

  static constexpr auto& kWaterOval = kInnerOval;
  static constexpr float kWaterRadius = kWaterOval.Height() / 2;

  // Constants describing the arrow around the condition symbol
  static constexpr float kMiddleR = (kInnerRadius + kSymbolRadius) / 2;
  static constexpr Rect kMiddleOval = Rect::MakeAtZero(kMiddleR * 2, kMiddleR * 2);
  static constexpr Rect kFarOval = kMiddleOval.Outset(kBorderHalf);
  static constexpr Rect kNearOval = kMiddleOval.Outset(-kBorderHalf);

  constexpr static float kRegionEndRadius = kGaugeRadius;
  constexpr static float kRegionStartRadius = kInnerRadius;
  constexpr static float kRegionWidth = kRegionEndRadius - kRegionStartRadius;
  constexpr static Rect kRegionOuter = Rect::MakeAtZero(2 * kRegionEndRadius, 2 * kRegionEndRadius);
  constexpr static Rect kRegionInner =
      Rect::MakeAtZero(2 * kRegionStartRadius, 2 * kRegionStartRadius);
  constexpr static float kRegionMargin = kBorderWidth / 2;

  virtual int KnobGet() const = 0;
  virtual void KnobSet(int value) = 0;

  Tock Tick(time::Timer& timer) override;

  virtual void DrawKnobBackground(SkCanvas& canvas, int value) const;
  virtual void DrawKnobSymbol(SkCanvas& canvas, int value) const = 0;

  // Allows derived classes to draw something under the glass
  virtual void DrawKnobBelowGlass(SkCanvas& canvas) const {}

  // Allows derived classes to draw something over the glass
  virtual void DrawKnobOverGlass(SkCanvas& canvas) const {}

  void Draw(SkCanvas& canvas) const override;

  Optional<Rect> DrawBounds() const override;

  struct ChangeEnumKnobAction : public Action {
    MortalPtr<EnumKnobWidget> widget;
    time::SteadyPoint start_time;
    ui::Pointer::IconOverride scroll_icon;

    ChangeEnumKnobAction(ui::Pointer& pointer, EnumKnobWidget& enum_knob_widget);
    void Update() override;
    ~ChangeEnumKnobAction();
  };

  std::unique_ptr<Action> FindAction(ui::Pointer& pointer, ui::ActionTrigger trigger) override;
};

}  // namespace automat::ui
