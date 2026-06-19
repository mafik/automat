#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include "widget.hpp"

namespace automat::ui {

// Reusable shadow effect
struct ShadowWidget : ui::Widget {
  float elevation = 0;
  float alpha = 1;
  ShadowWidget(Widget* parent);
  StrView Name() const override { return "Shadow"; }
  void SetElevation(float new_elevation) {
    elevation = new_elevation;
    WakeAnimation();
  }
  void SetAlpha(float new_alpha) {
    alpha = new_alpha;
    WakeAnimation();
  }
  // TODO: communicate the space occupied by the shadow somehow
  SkPath Shape() const override { return SkPath(); }
  Optional<Rect> TextureBounds() const override { return std::nullopt; }
  void Draw(SkCanvas&) const override;
};

}  // namespace automat::ui