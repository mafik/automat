#pragma once
// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT

#include "widget.hpp"

namespace automat::ui {

struct Text : Widget, PaintMixin {
  std::string text;
  Text(Widget* parent, std::string_view text = "");
  SkPath Shape() const override;
  void Draw(SkCanvas&) const override;
  StrView Name() const override { return "Text"; }
  Optional<Rect> DrawBounds() const override;
};

}  // namespace automat::ui