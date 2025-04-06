// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <include/core/SkVertices.h>

#include "color.hh"
#include "gui_button.hh"
#include "math.hh"
#include "units.hh"

// Utilities for creating Windows XP-style feel (a.k.a. "Luna").
namespace automat::theme::xp {

// TODO: look into Frutiger Aero

constexpr float kTitleBarHeight = 8_mm;
constexpr float kBorderWidth = 1_mm;

constexpr SkColor kFillColor = "#ece9d8"_color;
constexpr SkColor kBrightColor = "#3399ff"_color;
constexpr SkColor kMainColor = "#0066ff"_color;  // a beautiful electric blue
constexpr SkColor kDarkColor = "#0033cc"_color;
constexpr SkColor kDarkerColor = "#003399"_color;

constexpr Rect WindowBorderInner(Rect outer) {
  return Rect(outer.left + kBorderWidth, outer.bottom + kBorderWidth, outer.right - kBorderWidth,
              outer.top - kTitleBarHeight);
}

sk_sp<SkVertices> WindowBorder(Rect outer);

struct TitleButton : gui::Button {
  TitleButton(Ptr<Widget> child) : gui::Button(child) {}

  void DrawButtonShadow(SkCanvas& canvas, SkColor bg) const override {}
  void DrawButtonFace(SkCanvas&, SkColor bg, SkColor fg) const override;
  SkColor ForegroundColor() const override { return SK_ColorWHITE; }
  SkColor BackgroundColor() const override { return "#d4301f"_color; }
};

}  // namespace automat::theme::xp