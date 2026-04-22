// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <include/core/SkVertices.h>

#include "color.hh"
#include "math.hh"
#include "ui_button.hh"
#include "units.hh"

// Utilities for creating Windows XP-style feel (a.k.a. "Luna").
namespace automat::theme::xp {

// TODO: look into Frutiger Aero

constexpr float kTitleBarHeight = 8_mm;
constexpr float kBorderWidth = 1_mm;

constexpr int kTitleGridRows = 8;
constexpr int kTitleGridCornerCells = 3;
constexpr float kTitleGridCellSize = kTitleBarHeight / kTitleGridRows;

constexpr float kTitleCornerRadius = kTitleGridCellSize * kTitleGridCornerCells;

constexpr Rect WindowBorderInner(Rect outer) {
  return Rect(outer.left + kBorderWidth, outer.bottom + kBorderWidth, outer.right - kBorderWidth,
              outer.top - kTitleBarHeight);
}

sk_sp<SkVertices> WindowBorder(Rect outer, SkColor title_color = "#0066ff"_color);

struct TitleButton : ui::Button {
  TitleButton(ui::Widget* parent) : ui::Button(parent) {}

  void DrawButtonShadow(SkCanvas& canvas, SkColor4f bg) const override {}
  void DrawButtonFace(SkCanvas&, SkColor4f bg, SkColor4f fg) const override;
  SkColor4f ForegroundColor() const override { return SkColors::kWhite; }
  SkColor4f BackgroundColor() const override { return "#d4301f"_color4f; }
};

}  // namespace automat::theme::xp
