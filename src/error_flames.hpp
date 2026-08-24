#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include "widget.hpp"

namespace automat {

struct ErrorFlames : ui::Widget {
  float phase = 0;
  uint32_t parent_shape_genid = 0;
  Str text;
  float text_width = 0;

  ErrorFlames(ui::Widget& parent) : Widget(&parent) {}

  void SetText(StrView);

  SkPath Shape() const override;
  Optional<Rect> DrawBounds() const override;
  Tock Tick(time::Timer&) override;
  void Draw(SkCanvas&) const override;
};

}  // namespace automat
