#pragma once

// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include "widget.hh"

namespace automat {

struct TextWidget : ui::Widget {
  float width;
  Str text;
  TextWidget(ui::Widget* parent, Str text);
  Optional<Rect> TextureBounds() const override;
  SkPath Shape() const override;
  void Draw(SkCanvas& canvas) const override;
};

}  // namespace automat